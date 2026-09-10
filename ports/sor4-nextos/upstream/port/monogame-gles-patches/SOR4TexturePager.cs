using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using MonoGame.OpenGL;

namespace Microsoft.Xna.Framework.Graphics
{
    // Bounded texture residency for the SOR4 preload. The game keeps thousands of
    // Texture2D objects alive; on unified-memory handhelds that also keeps every GL
    // allocation alive. Native/pre-baked payloads are re-read from their original XNB;
    // only runtime-converted fallbacks use the append-only store. Cold GL storage is
    // replaced with a 1x1 placeholder and disk reads happen off the render thread.
    internal static class SOR4TexturePager
    {
        private sealed class Page
        {
            internal Texture2D Texture;
            internal long Offset;
            internal int Length;
            internal string Source;
            internal long LastUse;
            internal long ProtectedUntilFrame;
            internal bool Resident = true;
            internal bool Loading;
            internal bool Disposed;
            internal bool ReadFailureLogged;
            internal byte[] Ready;
        }

        private static readonly object Gate = new object();
        private static readonly Dictionary<Texture2D, Page> Pages = new Dictionary<Texture2D, Page>();
        private static readonly Queue<Page> Requests = new Queue<Page>();
        private static readonly Queue<Page> Ready = new Queue<Page>();
        private static readonly AutoResetEvent Wake = new AutoResetEvent(false);
        private static readonly bool Enabled = Environment.GetEnvironmentVariable("SOR4_PAGE") == "1";
        private static readonly bool Async = Environment.GetEnvironmentVariable("SOR4_PAGE_ASYNC") != "0";
        private static readonly bool Verbose = Environment.GetEnvironmentVariable("SOR4_PAGELOG") == "1";
        private static readonly long CapBytes = ReadMegabytes("SOR4_PAGE_CAP_MB", 96, 16, 2048);
        private static readonly long FloorBytes = ReadMegabytes("SOR4_PAGE_FLOOR_MB", 64, 0, 2048);
        private static readonly int MinimumBytes = (int)ReadKilobytes("SOR4_PAGE_MIN_KB", 48, 4, 65536);
        private static readonly int UploadsPerFrame = (int)ReadNumber("SOR4_PAGE_UPLOADS", 2, 1, 16);

        private static FileStream Store;
        private static Thread Worker;
        private static bool Initialized;
        private static bool Failed;
        private static long Clock;
        private static long Frame;
        private static long ResidentBytes;
        private static long Faults;
        private static long Evictions;
        private static long LastAvailableBytes = long.MaxValue;

        private static long ReadNumber(string name, long fallback, long minimum, long maximum)
        {
            long value;
            var text = Environment.GetEnvironmentVariable(name);
            if (string.IsNullOrEmpty(text) || !long.TryParse(text, out value)) value = fallback;
            if (value < minimum) value = minimum;
            if (value > maximum) value = maximum;
            return value;
        }

        private static long ReadMegabytes(string name, long fallback, long minimum, long maximum)
        {
            return ReadNumber(name, fallback, minimum, maximum) * 1024L * 1024L;
        }

        private static long ReadKilobytes(string name, long fallback, long minimum, long maximum)
        {
            return ReadNumber(name, fallback, minimum, maximum) * 1024L;
        }

        private static bool EnsureInitialized()
        {
            if (!Enabled || Failed) return false;
            lock (Gate)
            {
                if (Initialized) return true;
                try
                {
                    var directory = Environment.GetEnvironmentVariable("SOR4_PAGE_SWAP");
                    if (string.IsNullOrEmpty(directory)) throw new InvalidOperationException("SOR4_PAGE_SWAP is empty");
                    Directory.CreateDirectory(directory);
                    var path = Path.Combine(directory, "textures.pagepack");
                    Store = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.Read,
                        1024 * 1024, FileOptions.SequentialScan);
                    Initialized = true;
                    if (Async)
                    {
                        Worker = new Thread(WorkerLoop) { IsBackground = true, Name = "SOR4 texture pager" };
                        Worker.Start();
                    }
                    Console.Error.WriteLine("[SOR4-PAGE] enabled cap={0}MB floor={1}MB min={2}KB async={3}",
                        CapBytes / 1048576, FloorBytes / 1048576, MinimumBytes / 1024, Async ? 1 : 0);
                    return true;
                }
                catch (Exception error)
                {
                    Failed = true;
                    Console.Error.WriteLine("[SOR4-PAGE] disabled: " + error.Message);
                    return false;
                }
            }
        }

        internal static void Register(Texture2D texture, byte[] data, int length, string source)
        {
            if (texture == null || data == null || length < MinimumBytes || length > data.Length || !EnsureInitialized()) return;
            bool overBudget = false;
            lock (Gate)
            {
                if (Pages.ContainsKey(texture)) return;
                try
                {
                    long offset = -1;
                    if (string.IsNullOrEmpty(source)) {
                        Store.Position = Store.Length;
                        offset = Store.Position;
                        Store.Write(data, 0, length);
                    }
                    var page = new Page
                    {
                        Texture = texture,
                        Offset = offset,
                        Length = length,
                        Source = source,
                        LastUse = ++Clock
                    };
                    Pages.Add(texture, page);
                    ResidentBytes += length;
                    overBudget = ResidentBytes > CapBytes;
                }
                catch (Exception error)
                {
                    Failed = true;
                    Console.Error.WriteLine("[SOR4-PAGE] backing-store write failed: " + error.Message);
                }
            }
            // Preload can create many textures between two Present calls. Enforce
            // the hard cap here as well so one long loading frame cannot exhaust
            // unified RAM before normal per-frame maintenance gets a chance.
            if (overBudget) Threading.BlockOnUIThread(Trim);
        }

        internal static void Touch(Texture texture)
        {
            if (!Enabled || !(texture is Texture2D texture2D) || Failed) return;
            Page page;
            byte[] synchronous = null;
            bool readSynchronously = false;
            lock (Gate)
            {
                if (!Pages.TryGetValue(texture2D, out page) || page.Disposed) return;
                page.LastUse = ++Clock;
                // FrameEnd runs once in Present. Keep textures used by this draw alive
                // through that presentation so trim cannot replace storage that is
                // about to be sampled.
                page.ProtectedUntilFrame = Frame + 1;
                if (page.Resident || page.Loading || page.Ready != null) return;
                page.Loading = true;
                Faults++;
                if (Async)
                {
                    Requests.Enqueue(page);
                    Wake.Set();
                    return;
                }
                readSynchronously = true;
            }
            if (readSynchronously) {
                synchronous = ReadPage(page);
                lock (Gate)
                {
                    if (page.Disposed) synchronous = null;
                    page.Loading = false;
                }
            }
            if (synchronous != null) Upload(page, synchronous);
        }

        internal static void FrameEnd()
        {
            if (!Enabled || Failed || !Initialized) return;
            Frame++;
            for (var count = 0; count < UploadsPerFrame; count++)
            {
                Page page;
                byte[] data;
                lock (Gate)
                {
                    if (Ready.Count == 0) break;
                    page = Ready.Dequeue();
                    data = page.Ready;
                    page.Ready = null;
                    page.Loading = false;
                }
                if (data != null && !page.Disposed) Upload(page, data);
            }
            Trim();
            if (Verbose && Frame % 300 == 0)
            {
                lock (Gate)
                    Console.Error.WriteLine("[SOR4-PAGE] resident={0}MB cap={1}MB pages={2} faults={3} evictions={4} queue={5}/{6}",
                        ResidentBytes / 1048576, CapBytes / 1048576, Pages.Count, Faults, Evictions,
                        Requests.Count, Ready.Count);
            }
        }

        internal static void Forget(Texture texture)
        {
            if (!Enabled || !(texture is Texture2D texture2D)) return;
            lock (Gate)
            {
                Page page;
                if (!Pages.TryGetValue(texture2D, out page)) return;
                page.Disposed = true;
                if (page.Resident) ResidentBytes -= page.Length;
                Pages.Remove(texture2D);
            }
        }

        private static void WorkerLoop()
        {
            while (true)
            {
                Page page = null;
                lock (Gate)
                {
                    if (Requests.Count > 0) page = Requests.Dequeue();
                }
                if (page == null)
                {
                    Wake.WaitOne(1000);
                    continue;
                }
                bool disposed;
                lock (Gate) disposed = page.Disposed;
                byte[] data = disposed ? null : ReadPage(page);
                lock (Gate)
                {
                    if (page.Disposed)
                    {
                        page.Loading = false;
                        continue;
                    }
                    page.Ready = data;
                    if (data != null) Ready.Enqueue(page);
                    else page.Loading = false;
                }
            }
        }

        private static byte[] ReadPage(Page page)
        {
            try
            {
                if (!string.IsNullOrEmpty(page.Source))
                {
                    var sourceData = Microsoft.Xna.Framework.Content.Texture2DReader.Sor4ReadPage(
                        page.Source, (int)page.Texture.Format, page.Length);
                    if (sourceData == null && Verbose && !page.ReadFailureLogged)
                    {
                        page.ReadFailureLogged = true;
                        Console.Error.WriteLine("[SOR4-PAGE] source rejected asset={0} format={1} bytes={2}",
                            page.Source, (int)page.Texture.Format, page.Length);
                    }
                    return sourceData;
                }
                var data = new byte[page.Length];
                lock (Gate) {
                    var old = Store.Position;
                    Store.Position = page.Offset;
                    var offset = 0;
                    while (offset < data.Length)
                    {
                        var read = Store.Read(data, offset, data.Length - offset);
                        if (read <= 0) throw new EndOfStreamException();
                        offset += read;
                    }
                    Store.Position = old;
                }
                return data;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("[SOR4-PAGE] read failed: " + error.Message);
                return null;
            }
        }

        private static void Upload(Page page, byte[] data)
        {
            try
            {
                if (page.Texture.IsDisposed) return;
                page.Texture.SetData(0, null, data, 0, data.Length);
                lock (Gate)
                {
                    if (!page.Resident)
                    {
                        page.Resident = true;
                        ResidentBytes += page.Length;
                    }
                }
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("[SOR4-PAGE] upload failed: " + error.Message);
            }
        }

        private static long MemAvailableBytes()
        {
            if (FloorBytes == 0) return long.MaxValue;
            try
            {
                foreach (var line in File.ReadLines("/proc/meminfo"))
                {
                    if (!line.StartsWith("MemAvailable:")) continue;
                    var fields = line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                    long value;
                    if (fields.Length > 1 && long.TryParse(fields[1], out value)) return value * 1024L;
                }
            }
            catch { }
            return long.MaxValue;
        }

        private static void Trim()
        {
            long target;
            lock (Gate)
            {
                target = CapBytes;
                // /proc parsing on every draw was measurable on Cortex-A35. This
                // method now runs once per presented frame and samples pressure at
                // 2 Hz; the hard residency cap is still enforced every frame.
                if (Frame % 30 == 1) LastAvailableBytes = MemAvailableBytes();
                var available = LastAvailableBytes;
                if (available < FloorBytes)
                {
                    var pressureTarget = Math.Max(16L * 1024L * 1024L, ResidentBytes - 16L * 1024L * 1024L);
                    if (pressureTarget < target) target = pressureTarget;
                }
            }
            while (true)
            {
                Page victim = null;
                lock (Gate)
                {
                    if (ResidentBytes <= target) return;
                    foreach (var page in Pages.Values)
                    {
                        if (!page.Resident || page.Loading || page.Disposed ||
                            page.ProtectedUntilFrame >= Frame) continue;
                        if (victim == null || page.LastUse < victim.LastUse) victim = page;
                    }
                    if (victim == null) return;
                    victim.Resident = false;
                    ResidentBytes -= victim.Length;
                    Evictions++;
                }
                Evict(victim);
            }
        }

        private static void Evict(Page page)
        {
            try
            {
                var texture = page.Texture;
                if (texture == null || texture.IsDisposed || texture.glTexture <= 0) return;
                var previous = GraphicsExtensions.GetBoundTexture2D();
                GL.BindTexture(TextureTarget.Texture2D, texture.glTexture);
                GL.TexImage2D(TextureTarget.Texture2D, 0, PixelInternalFormat.Rgba,
                    1, 1, 0, PixelFormat.Rgba, PixelType.UnsignedByte, IntPtr.Zero);
                if (previous != texture.glTexture) GL.BindTexture(TextureTarget.Texture2D, previous);
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("[SOR4-PAGE] eviction failed: " + error.Message);
            }
        }
    }
}
