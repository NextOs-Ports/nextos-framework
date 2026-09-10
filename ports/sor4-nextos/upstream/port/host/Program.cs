using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Threading;

class Host
{
    const int StarterBusy = 75;
    static readonly object ProcessGate = new();
    static Process ActiveChild;
    static Process ControlsChild;
    static volatile bool StopRequested;

    static string HostDir => AppContext.BaseDirectory;
    static string GameDir => Directory.GetParent(HostDir.TrimEnd(Path.DirectorySeparatorChar))!.FullName;
    static string HostPath => Path.Combine(HostDir, "sor4host");

    [DllImport("libc", SetLastError = true)] static extern int kill(int pid, int signal);

    static void Log(string text)
    {
        Console.Error.WriteLine("[host] " + text);
        Console.Error.Flush();
    }

    static void ConfigureResolver()
    {
        AssemblyLoadContext.Default.Resolving += (context, name) =>
        {
            string simple = name.Name!;
            if (simple.StartsWith("System.", StringComparison.Ordinal) ||
                simple.StartsWith("Microsoft.", StringComparison.Ordinal) ||
                simple is "System" or "netstandard" or "mscorlib" or "WindowsBase")
                return null;
            string path = Path.Combine(HostDir, simple + ".dll");
            if (File.Exists(path))
                return context.LoadFromAssemblyPath(path);
            Log("unresolved assembly: " + name.FullName);
            return null;
        };
        AppDomain.CurrentDomain.UnhandledException += (_, eventArgs) =>
            Log("UNHANDLED: " + eventArgs.ExceptionObject);
    }

    static int Main(string[] args)
    {
        ConfigureResolver();
        if (args.Length >= 2 && args[0] == "--run-dll")
            return RunTool(args);
        if (args.Length == 1 && args[0] == "--game")
            return RunGame();
        if (args.Length == 0 || (args.Length == 1 && args[0] == "--starter"))
            return RunStarter();
        Log("usage: sor4host --starter | --game | --run-dll TOOL.dll [args]");
        return 2;
    }

    static int RunTool(string[] args)
    {
        string dll = Path.IsPathRooted(args[1]) ? args[1] : Path.Combine(HostDir, args[1]);
        string[] toolArgs = args.Skip(2).ToArray();
        Log("--run-dll " + dll + " [" + string.Join(" ", toolArgs) + "]");
        try
        {
            Assembly assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(dll));
            MethodInfo entry = assembly.EntryPoint ?? throw new InvalidOperationException("tool has no entry point");
            object result = entry.Invoke(null, entry.GetParameters().Length == 1 ? new object[] { toolArgs } : null);
            return result is int status ? status : 0;
        }
        catch (TargetInvocationException error)
        {
            Log("tool exception: " + (error.InnerException ?? error));
            return 4;
        }
        catch (Exception error)
        {
            Log("tool start failed: " + error);
            return 3;
        }
    }

    static int RunGame()
    {
        try
        {
            Log("loading SOR4.dll");
            Assembly sor4 = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(HostDir, "SOR4.dll"));
            sor4.GetType("CommonLib.utils", true)!
                .GetMethod("set_as_main_thread", BindingFlags.Public | BindingFlags.Static)!
                .Invoke(null, null);
            Log("set_as_main_thread OK");

            Type program = sor4.GetType("BeatThemAll.MetaGame.program", true)!;
            program.GetMethod("static_init", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)!
                .Invoke(null, null);
            Log("program.static_init OK");

            Type reflection = sor4.GetType("CommonLib.reflection", true)!;
            Type activity = sor4.GetType("SOR4.Android.MainActivity", true)!;
            foreach (string name in new[] { "delegate_serialize", "delegate_deserialize", "delegate_deep_clone" })
            {
                FieldInfo field = reflection.GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)!;
                MethodInfo method = activity.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)!;
                field.SetValue(null, Delegate.CreateDelegate(field.FieldType, method));
            }

            Type xna = sor4.GetType("CommonLib.xna", true)!;
            xna.GetMethod("CreateGame", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);
            var game = (Microsoft.Xna.Framework.Game)xna
                .GetField("game", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)!
                .GetValue(null)!;
            Log("Run()");
            game.Run();
            Log("Run returned");
            return 0;
        }
        catch (Exception error)
        {
            Log("game exception: " + error);
            if (error.InnerException != null)
                Log("inner: " + error.InnerException);
            return 1;
        }
    }

    static int RunStarter()
    {
        if (!File.Exists(HostPath) || !Directory.Exists(GameDir))
        {
            Log("starter could not resolve the installed game directory");
            return 1;
        }
        if (SetupIsActive())
        {
            Log("setup is already active; leaving it untouched");
            return StarterBusy;
        }
        if (AnotherHostIsRunning())
        {
            Log("another SOR4 host instance is active");
            return StarterBusy;
        }

        string runLock = Path.Combine(GameDir, ".sor4.run.lock");
        FileStream lockStream;
        try
        {
            if (Directory.Exists(runLock))
                runLock = Path.Combine(GameDir, ".sor4.run.lock.file");
            lockStream = new FileStream(runLock, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            lockStream.SetLength(0);
            using (var writer = new StreamWriter(lockStream, System.Text.Encoding.ASCII, 128, true))
            {
                writer.WriteLine(Environment.ProcessId);
                writer.Flush();
            }
            lockStream.Flush(true);
        }
        catch (IOException)
        {
            Log("another launcher owns the run lock");
            return StarterBusy;
        }

        ResetRunLog();

        string compatDir = null;
        string privateRuntime = null;
        using (lockStream)
        using (PosixSignalRegistration term = RegisterSignal(PosixSignal.SIGTERM))
        using (PosixSignalRegistration interrupt = RegisterSignal(PosixSignal.SIGINT))
        using (PosixSignalRegistration hangup = RegisterSignal(PosixSignal.SIGHUP))
        {
            Console.CancelKeyPress += OnCancelKeyPress;
            try
            {
                if (!LoadProfile())
                    return 1;
                if (!PrepareRuntime(out compatDir, out privateRuntime))
                    return 1;
                if (RunSetup() != 0)
                    return 1;
                if (StopRequested)
                    return 130;

                ControlsChild = StartControls();
                int status = RunChild(HostPath, new[] { "--game" }, GameDir);
                return StopRequested && status == 0 ? 130 : status;
            }
            finally
            {
                StopTrackedProcess(ControlsChild);
                ControlsChild = null;
                Console.CancelKeyPress -= OnCancelKeyPress;
                RemovePrivateDirectory(compatDir, "sor4-compat.");
                RemovePrivateDirectory(privateRuntime, "sor4-xdg.");
            }
        }
    }

    // The launcher already truncated log.txt and still holds the descriptor this
    // process inherited; rewriting the file here would desynchronise both offsets.
    static void ResetRunLog() =>
        Console.Error.WriteLine("[port] Streets of Rage 4 2.0");

    static PosixSignalRegistration RegisterSignal(PosixSignal signal) =>
        PosixSignalRegistration.Create(signal, context =>
        {
            context.Cancel = true;
            StopRequested = true;
            Process child;
            lock (ProcessGate) child = ActiveChild;
            StopTrackedProcess(child);
        });

    static void OnCancelKeyPress(object sender, ConsoleCancelEventArgs eventArgs)
    {
        eventArgs.Cancel = true;
        StopRequested = true;
        Process child;
        lock (ProcessGate) child = ActiveChild;
        StopTrackedProcess(child);
    }

    static bool SetupIsActive()
    {
        string pidPath = Path.Combine(GameDir, ".setup.lock", "pid");
        if (!int.TryParse(ReadFirstLine(pidPath), out int pid) || pid <= 0 || kill(pid, 0) != 0)
            return false;
        string command = ReadProcCommandLine(pid);
        return command.Contains("sor4_setup.sh", StringComparison.Ordinal);
    }

    // A previous run that ended badly can leave a host holding the framebuffer or the DRM
    // master, and every later launch would then open black.  An orphan (its launcher is
    // gone, so it was reparented to init) is killed; a host whose launcher is still alive
    // is a real second instance and this launch steps aside instead.
    static bool AnotherHostIsRunning()
    {
        string self;
        try { self = Path.GetFullPath(HostPath); }
        catch { return false; }
        var hosts = new Dictionary<int, int>();
        foreach (string directory in Directory.EnumerateDirectories("/proc"))
        {
            if (!int.TryParse(Path.GetFileName(directory), out int pid) || pid == Environment.ProcessId)
                continue;
            try
            {
                FileSystemInfo target = File.ResolveLinkTarget(Path.Combine(directory, "exe"), true)!;
                if (target == null || Path.GetFullPath(target.FullName) != self)
                    continue;
            }
            catch { continue; }
            // A parent we cannot read means the process just exited; never signal that
            // pid, because the number may already belong to something unrelated.
            int parent = ReadParentPid(pid);
            if (parent >= 0) hosts[pid] = parent;
        }

        // A game child whose own starter is being reaped is orphaned too, so classify the
        // whole set before killing anything.
        var orphans = new HashSet<int>(hosts.Where(entry => entry.Value <= 1).Select(entry => entry.Key));
        for (bool grew = true; grew;)
        {
            grew = false;
            foreach (var entry in hosts)
                if (!orphans.Contains(entry.Key) && orphans.Contains(entry.Value))
                    grew = orphans.Add(entry.Key);
        }
        foreach (int pid in orphans)
        {
            Log($"reaping orphaned host pid={pid} left by an earlier session");
            kill(pid, 9);
        }
        return hosts.Keys.Any(pid => !orphans.Contains(pid));
    }

    static int ReadParentPid(int pid)
    {
        try
        {
            foreach (string line in File.ReadLines($"/proc/{pid}/status"))
            {
                if (!line.StartsWith("PPid:", StringComparison.Ordinal)) continue;
                return int.Parse(line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries)[1]);
            }
        }
        catch { }
        return -1;
    }

    static bool LoadProfile()
    {
        string profile = Path.Combine(GameDir, "tools", "sor4_profile.sh");
        if (!File.Exists(profile))
        {
            Log("missing tools/sor4_profile.sh");
            return false;
        }
        var allowed = new HashSet<string>(new[]
        {
            "SOR4_GLES", "SOR4_CLEAR_VIDEODRIVER", "SOR4_MEMTOTAL_KB", "SOR4_PROFILE_EFFECTIVE", "SOR4_TEXTURE_MODE",
            "SOR4_PAGE", "SOR4_PAGE_ASYNC", "SOR4_PAGE_SWAP", "SOR4_PAGE_CAP_MB",
            "SOR4_PAGE_FLOOR_MB", "SOR4_PAGE_MIN_KB", "SOR4_PAGE_UPLOADS", "SOR4_PAGELOG",
            "SOR4_NATIVE_ASTC", "SOR4_ETC1", "SOR4_BAKE_SCALE", "SOR4_TEXSCALE",
            "SOR4_CONV_THREADS", "MALLOC_ARENA_MAX", "DOTNET_GCConserveMemory", "DOTNET_TieredPGO"
        }, StringComparer.Ordinal);
        var start = new ProcessStartInfo("/bin/bash")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            WorkingDirectory = GameDir
        };
        start.ArgumentList.Add(profile);
        start.ArgumentList.Add("--emit");
        start.ArgumentList.Add(GameDir);
        using Process process = Process.Start(start)!;
        string line;
        int exports = 0;
        while ((line = process.StandardOutput.ReadLine()) != null)
        {
            if (!line.StartsWith("SOR4_EXPORT ", StringComparison.Ordinal))
            {
                Console.Error.WriteLine(line);
                continue;
            }
            string assignment = line.Substring("SOR4_EXPORT ".Length);
            int separator = assignment.IndexOf('=');
            if (separator <= 0)
                continue;
            string key = assignment.Substring(0, separator);
            string value = assignment.Substring(separator + 1);
            if (!allowed.Contains(key) || value.IndexOfAny(new[] { '\0', '\r', '\n' }) >= 0)
                continue;
            Environment.SetEnvironmentVariable(key, value);
            exports++;
        }
        process.WaitForExit();
        if (process.ExitCode != 0 || exports != allowed.Count)
        {
            Log($"profile selection failed (status={process.ExitCode}, exports={exports}/{allowed.Count})");
            return false;
        }
        return true;
    }

    static bool PrepareRuntime(out string compatDir, out string privateRuntime)
    {
        compatDir = null;
        privateRuntime = null;
        string libs = Path.Combine(HostDir, "libs");
        // Firmware copies win; the bundled pair is only consulted when the CFW has none.
        string harfbuzz = FindNative("libharfbuzz.so.0") ?? FindNative("libharfbuzz.so") ??
            FindFallback("libharfbuzz.so.0");
        string freetype = FindNative("libfreetype.so.6") ?? FindNative("libfreetype.so") ??
            FindFallback("libfreetype.so.6");
        var missing = new List<string>();
        if (harfbuzz == null) missing.Add("libharfbuzz.so.0");
        if (freetype == null) missing.Add("libfreetype.so.6");
        foreach (string name in new[] { "libopenal.so.1", "libopusfile.so.0", "libopus.so.0", "libogg.so.0" })
            if (!File.Exists(Path.Combine(libs, name))) missing.Add(name);

        string sdlPrefix = "";
        if (FindNative("libSDL3.so.0", false) != null)
        {
            string bridge = Path.Combine(HostDir, "sdl3compat");
            if (!File.Exists(Path.Combine(bridge, "libSDL2-2.0.so.0")))
                missing.Add("sdl2-compat");
            else
                sdlPrefix = bridge;
        }
        else if (FindNative("libSDL2-2.0.so.0") == null && FindNative("libSDL2.so") == null)
            missing.Add("libSDL2-2.0.so.0");
        if (missing.Count != 0)
        {
            Log("missing native dependencies: " + string.Join(" ", missing));
            return false;
        }

        if (!CreateCompatibilityDirectory(harfbuzz!, freetype!, out compatDir, out privateRuntime))
        {
            Log("could not create private FreeType/HarfBuzz aliases");
            return false;
        }

        string inherited = Environment.GetEnvironmentVariable("LD_LIBRARY_PATH") ?? "";
        var paths = new List<string>();
        if (sdlPrefix.Length != 0) paths.Add(sdlPrefix);
        paths.Add(compatDir);
        paths.Add(libs);
        paths.AddRange(new[] { "/usr/local/lib/aarch64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu", "/usr/lib", "/lib" });
        paths.AddRange(ControlFolderLibraryRoots());
        if (inherited.Length != 0) paths.Add(inherited);
        Set("LD_LIBRARY_PATH", string.Join(':', paths));
        SetDefault("SDL_NO_SIGNAL_HANDLERS", "1");
        SetDefault("SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP", "1");
        SetDefault("SDL_VIDEO_FULLSCREEN_DESKTOP", "1");
        // Mesa-based firmwares answer an ES request with a desktop-GL context unless SDL
        // is told to load the ES driver; GLSL ES shaders then fail to compile and the
        // screen stays black.  This selects an API, never a display backend.
        SetDefault("SDL_OPENGL_ES_DRIVER", "1");
        ApplyVideoDriverDecision();
        SetDefault("DOTNET_EnableWriteXorExecute", "0");
        SetDefault("DOTNET_gcServer", "0");

        string save = Path.Combine(GameDir, "save");
        string textureCache = Path.Combine(GameDir, "texcache");
        Directory.CreateDirectory(save);
        Directory.CreateDirectory(textureCache);
        string previousHome = Environment.GetEnvironmentVariable("HOME");
        Set("HOME", save);
        Set("SOR4_ASSETS", Path.Combine(GameDir, "gameassets"));
        Set("SOR4_TEXCACHE", textureCache);
        SetDefault("SOR4_TEXSCALE", "3");
        SetDefault("CUP_NOSIGH", "1");
        SetDefault("CUP_GCSIG", "1");
        Set("SOR4_AUDIO", Path.Combine(GameDir, "audioout"));
        Set("SOR4_BANKDIR", Path.Combine(GameDir, "gameassets"));
        Set("WWISE_REAL", Path.Combine(libs, "libWwise.real.so"));
        string wwiseLog = Path.Combine(GameDir, "wwise.log");
        Set("WWISE_LOG", wwiseLog);
        try
        {
            // One file per launch: old sessions made field reports look like one
            // enormous run and hid whether a problem had actually regressed.
            File.WriteAllText(wwiseLog, string.Empty);
        }
        catch (Exception error)
        {
            Log($"warning: could not reset wwise.log: {error.Message}");
        }
        SetDefault("WWISE_TICK_US", "33333");
        SetDefault("SOR4_MUSIC_GRACE", "3600");
        SetDefault("SOR4_SFXGAIN", "1.1");
        SetDefault("SOR4_MUSICGAIN", "0.6");
        string alsoft = Path.Combine(GameDir, "alsoft.conf");
        if (Environment.GetEnvironmentVariable("ALSOFT_CONF") is null && File.Exists(alsoft))
            Set("ALSOFT_CONF", alsoft);
        KeepAlsaConfigurationReachable(previousHome, save);
        DetectPulseServer();
        Log($"native aliases ready: FreeType={freetype} HarfBuzz={harfbuzz}");
        return true;
    }

    static string FindNative(string name, bool includePackage = true)
    {
        var roots = new List<string>();
        if (includePackage) roots.Add(Path.Combine(HostDir, "libs"));
        roots.AddRange(new[]
        {
            "/usr/local/lib/aarch64-linux-gnu", "/usr/local/lib", "/lib/aarch64-linux-gnu",
            "/usr/lib/aarch64-linux-gnu", "/lib64", "/usr/lib64", "/lib", "/usr/lib"
        });
        // Lean firmwares keep their compatibility libraries in PortMaster's own
        // directory rather than in the system prefixes.
        roots.AddRange(ControlFolderLibraryRoots());
        return roots.Select(root => Path.Combine(root, name)).FirstOrDefault(File.Exists);
    }

    static IEnumerable<string> ControlFolderLibraryRoots()
    {
        string controlFolder = Environment.GetEnvironmentVariable("SOR4_CONTROLFOLDER");
        if (string.IsNullOrWhiteSpace(controlFolder) || !Directory.Exists(controlFolder))
            yield break;
        foreach (string name in new[] { "libs", "libs.aarch64" })
        {
            string root = Path.Combine(controlFolder, name);
            if (Directory.Exists(root)) yield return root;
        }
    }

    // Last resort only: a dependency-free FreeType/HarfBuzz pair shipped with the port
    // so a firmware that ships neither can still start instead of refusing to launch.
    static string FindFallback(string name)
    {
        string path = Path.Combine(HostDir, "libs", "fallback", name);
        return File.Exists(path) ? path : null;
    }

    static bool CreateCompatibilityDirectory(string harfbuzz, string freetype,
        out string compatDir, out string privateRuntime)
    {
        compatDir = null;
        privateRuntime = null;
        var candidates = new List<string>();
        string inherited = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!string.IsNullOrWhiteSpace(inherited)) candidates.Add(inherited);
        int uid = ReadUid();
        candidates.Add("/run/user/" + uid);
        candidates.Add("/var/run/user/" + uid);
        foreach (string candidate in candidates.Distinct())
        {
            if (!Directory.Exists(candidate)) continue;
            string attempt = Path.Combine(candidate, $"sor4-compat.{Environment.ProcessId}.{Guid.NewGuid():N}");
            if (TryCreateAliases(attempt, harfbuzz, freetype))
            {
                compatDir = attempt;
                Set("XDG_RUNTIME_DIR", candidate);
                return true;
            }
        }

        string root = Path.Combine("/tmp", $"sor4-xdg.{Environment.ProcessId}.{Guid.NewGuid():N}");
        try
        {
            Directory.CreateDirectory(root);
            File.SetUnixFileMode(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            string attempt = Path.Combine(root, $"sor4-compat.{Environment.ProcessId}");
            if (!TryCreateAliases(attempt, harfbuzz, freetype))
            {
                Directory.Delete(root, true);
                return false;
            }
            privateRuntime = root;
            compatDir = attempt;
            Set("XDG_RUNTIME_DIR", root);
            return true;
        }
        catch { return false; }
    }

    static bool TryCreateAliases(string directory, string harfbuzz, string freetype)
    {
        try
        {
            Directory.CreateDirectory(directory);
            File.SetUnixFileMode(directory, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            foreach (string alias in new[] { "libharfbuzz", "libharfbuzz.so", "libharfbuzz.so.0" })
                File.CreateSymbolicLink(Path.Combine(directory, alias), harfbuzz);
            foreach (string alias in new[] { "freetype", "freetype.so", "libfreetype.so", "libfreetype.so.6" })
                File.CreateSymbolicLink(Path.Combine(directory, alias), freetype);
            return true;
        }
        catch
        {
            try { if (Directory.Exists(directory)) Directory.Delete(directory, true); } catch { }
            return false;
        }
    }

    // The probe reports an inherited SDL_VIDEODRIVER that this firmware's SDL cannot
    // initialise.  Removing it lets SDL auto-detect; the port never names a backend
    // itself, because the working one differs per device (fbdev, mali, kmsdrm, wayland).
    static void ApplyVideoDriverDecision()
    {
        if (Environment.GetEnvironmentVariable("SOR4_CLEAR_VIDEODRIVER") != "1") return;
        string inherited = Environment.GetEnvironmentVariable("SDL_VIDEODRIVER");
        if (string.IsNullOrEmpty(inherited)) return;
        Set("SDL_VIDEODRIVER", null);
        Log($"SDL_VIDEODRIVER={inherited} nao inicializa aqui; deixando o SDL escolher");
    }

    // The game keeps its settings under save/, so HOME is redirected there before the
    // audio stack starts.  ALSA reads ~/.asoundrc, which on Knulli/Batocera/muOS is the
    // only place the working PCM (and the software volume control) is defined: without
    // this link the redirect would silence the port on exactly those firmwares.
    static void KeepAlsaConfigurationReachable(string previousHome, string save)
    {
        string destination = Path.Combine(save, ".asoundrc");
        if (Directory.Exists(destination)) return;

        var candidates = new List<string>();
        if (!string.IsNullOrWhiteSpace(previousHome)) candidates.Add(previousHome);
        candidates.AddRange(new[] { "/userdata/system", "/storage/.config", "/root", "/home/ark" });
        string source = candidates
            .Select(root => Path.Combine(root, ".asoundrc"))
            .FirstOrDefault(File.Exists);
        string controlFolderConfig = ControlFolderLibraryRoots()
            .Select(root => Path.Combine(root, "asound.conf"))
            .FirstOrDefault(File.Exists);
        source ??= controlFolderConfig;
        if (source == null || SameContent(source, destination)) return;

        // The port directory usually lives on the exFAT/FAT card, which rejects symlinks
        // outright (ENOTSUPP), so a refreshed copy is the portable form of the same link.
        try
        {
            if (File.Exists(destination)) File.Delete(destination);
            File.CreateSymbolicLink(destination, source);
            Log("ALSA: " + source + " visivel para o novo HOME");
            return;
        }
        catch (Exception)
        {
        }
        try
        {
            File.Copy(source, destination, true);
            Log("ALSA: " + source + " copiado para o novo HOME (link nao suportado aqui)");
        }
        catch (Exception error)
        {
            Log("could not expose " + source + ": " + error.Message);
        }
    }

    static bool SameContent(string source, string destination)
    {
        try
        {
            if (!File.Exists(destination)) return false;
            return File.ReadAllBytes(source).AsSpan().SequenceEqual(File.ReadAllBytes(destination));
        }
        catch { return false; }
    }

    static void DetectPulseServer()
    {
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PULSE_SERVER"))) return;
        string runtime = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR") ?? "";
        foreach (string socket in new[] { "/run/pulse/native", "/var/run/pulse/native", Path.Combine(runtime, "pulse", "native") })
        {
            if (File.Exists(socket))
            {
                Set("PULSE_SERVER", "unix:" + socket);
                return;
            }
        }
    }

    static int RunSetup()
    {
        string setup = Path.Combine(GameDir, "tools", "sor4_setup.sh");
        if (!File.Exists(setup))
        {
            Log("missing tools/sor4_setup.sh");
            return 1;
        }
        Log("validating/installing the transactional game payload");
        return RunChild("/bin/bash", new[] { setup }, GameDir);
    }

    static int RunChild(string executable, IEnumerable<string> arguments, string workingDirectory)
    {
        var start = new ProcessStartInfo(executable)
        {
            UseShellExecute = false,
            WorkingDirectory = workingDirectory
        };
        foreach (string argument in arguments) start.ArgumentList.Add(argument);
        using Process process = Process.Start(start)!;
        lock (ProcessGate) ActiveChild = process;
        try
        {
            process.WaitForExit();
            return process.ExitCode;
        }
        finally
        {
            lock (ProcessGate) if (ReferenceEquals(ActiveChild, process)) ActiveChild = null;
        }
    }

    static Process StartControls()
    {
        if (Environment.GetEnvironmentVariable("SOR4_USE_GPTK") != "1") return null;
        string command = Environment.GetEnvironmentVariable("GPTOKEYB");
        if (string.IsNullOrWhiteSpace(command) || (!File.Exists(command) && Which(command) == null))
            command = Which("gptokeyb");
        if (command == null)
        {
            Log("SOR4_USE_GPTK=1 but gptokeyb is unavailable");
            return null;
        }
        var start = new ProcessStartInfo(command) { UseShellExecute = false, WorkingDirectory = GameDir };
        start.ArgumentList.Add("sor4host");
        start.ArgumentList.Add("-c");
        start.ArgumentList.Add(Path.Combine(GameDir, "sor4.gptk"));
        try { return Process.Start(start); }
        catch (Exception error) { Log("could not start gptokeyb: " + error.Message); return null; }
    }

    static string Which(string command)
    {
        if (string.IsNullOrWhiteSpace(command) || command.Contains(Path.DirectorySeparatorChar))
            return File.Exists(command) ? command : null;
        foreach (string root in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':'))
        {
            string candidate = Path.Combine(root, command);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    static void StopTrackedProcess(Process process)
    {
        if (process == null) return;
        try
        {
            if (process.HasExited) return;
            kill(process.Id, 15);
            if (!process.WaitForExit(5000) && !process.HasExited)
                process.Kill(false);
        }
        catch { }
    }

    static void RemovePrivateDirectory(string path, string requiredPrefix)
    {
        if (string.IsNullOrEmpty(path)) return;
        try
        {
            string name = Path.GetFileName(path);
            if (name.StartsWith(requiredPrefix, StringComparison.Ordinal) && Directory.Exists(path))
                Directory.Delete(path, true);
        }
        catch { }
    }

    static string ReadFirstLine(string path)
    {
        try { using var reader = new StreamReader(path); return reader.ReadLine() ?? ""; }
        catch { return ""; }
    }

    static string ReadProcCommandLine(int pid)
    {
        try { return File.ReadAllText($"/proc/{pid}/cmdline").Replace('\0', ' '); }
        catch { return ""; }
    }

    static int ReadUid()
    {
        try
        {
            string line = File.ReadLines("/proc/self/status").First(value => value.StartsWith("Uid:", StringComparison.Ordinal));
            return int.Parse(line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries)[1]);
        }
        catch { return 0; }
    }

    static void Set(string name, string value) => Environment.SetEnvironmentVariable(name, value);
    static void SetDefault(string name, string value)
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable(name))) Set(name, value);
    }
}
