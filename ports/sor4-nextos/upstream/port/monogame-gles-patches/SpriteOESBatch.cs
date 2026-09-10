// SOR4: tipo custom do fork MonoGame Android. No Android desenha texturas OES externas
// (frame de video da Surface) via um SpriteBatch especializado. Aqui a intro e pulada
// (ver SuperVideoPlayer), entao platform.video_draw() nunca chega a usar este batch para
// desenhar — basta ser um SpriteBatch valido para o `newobj` em platform.video_start().
using Microsoft.Xna.Framework.Graphics;

namespace Microsoft.Xna.Framework.Graphics
{
    public class SpriteOESBatch : SpriteBatch
    {
        public SpriteOESBatch(GraphicsDevice graphicsDevice) : base(graphicsDevice) { }
    }
}
