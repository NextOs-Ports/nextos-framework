// SOR4: tipo custom do fork MonoGame Android usado por CommonLib.platform para tocar
// os videos de intro (.mp4) via MediaPlayer/Surface OES do Android. O Mali-450 nao tem
// decode de video, e o objetivo do port e PULAR a intro e cair no menu (TitleScreen).
//
// Por isso este stub reporta o video como "ja terminado" no 1o frame:
//   State = Stopped (0)  -> platform.video_draw() retorna cedo (nao chama GetTexture)
//   PlayPosition = Duration = 1s (>0) -> platform.video_is_finished() retorna true
// Com isso StartGameVideoScreen.handle_input() avanca por todos os intros e vai pro menu.
using System;
using Microsoft.Xna.Framework.Graphics;

namespace Microsoft.Xna.Framework.Media
{
    public class SuperVideoPlayer : IDisposable
    {
        private static readonly TimeSpan _one = TimeSpan.FromSeconds(1);

        public SuperVideoPlayer() { }

        public void Play(string fileName)
        {
            System.Console.Error.WriteLine("[SVP] Play(" + fileName + ") -> stub: video pulado");
        }

        public void Pause() { }
        public void Resume() { }

        // Sempre "parado": video_draw() ve State==Stopped e retorna sem desenhar/GetTexture.
        public MediaState State { get { return MediaState.Stopped; } }

        // Nunca chamado (video_draw retorna antes quando State==Stopped), mas precisa existir.
        public Texture2D GetTexture() { return null; }

        // video_is_finished(): nao-zero e PlayPosition>=Duration -> "terminado" no 1o frame.
        public TimeSpan PlayPosition { get { return _one; } }
        public TimeSpan Duration { get { return _one; } }

        public void Dispose() { }
    }
}
