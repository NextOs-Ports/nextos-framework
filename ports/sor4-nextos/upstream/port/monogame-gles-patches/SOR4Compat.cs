using System.Runtime.CompilerServices;
namespace Microsoft.Xna.Framework
{
    public class AndroidGameActivity : Android.App.Activity
    {
        public AndroidGameActivity() { }
        public override Android.Content.Res.AssetManager Assets
        {
            get { System.Console.Error.WriteLine("[MG] AndroidGameActivity.Assets override chamado"); System.Console.Error.Flush(); return SOR4Bridge.AssetBridge.GetAssets(); }
        }
    }
    public partial class Game
    {
        private static AndroidGameActivity _sor4Activity;
        public static AndroidGameActivity Activity
        {
            get
            {
                if (_sor4Activity == null)
                {
                    System.Console.Error.WriteLine("[MG] Game.Activity: criando AndroidGameActivity"); System.Console.Error.Flush();
                    _sor4Activity = (AndroidGameActivity)RuntimeHelpers.GetUninitializedObject(typeof(AndroidGameActivity));
                    System.Console.Error.WriteLine("[MG] Game.Activity: criado ok"); System.Console.Error.Flush();
                }
                return _sor4Activity;
            }
            internal set { _sor4Activity = value; }
        }
    }
}
