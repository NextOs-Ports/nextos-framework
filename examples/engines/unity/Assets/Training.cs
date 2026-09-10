// SPDX-License-Identifier: GPL-3.0-only
using UnityEngine;
public sealed class Training : MonoBehaviour {
    Vector2 position=new Vector2(40,240);
    Texture2D pixel;
    int score;
    void Awake() {
        pixel=new Texture2D(1,1,TextureFormat.RGBA32,false);
        pixel.SetPixel(0,0,Color.white);pixel.Apply();
        score=PlayerPrefs.GetInt("nextos_training_score",0);
    }
    void Update() {
        var movement=new Vector2(Input.GetAxisRaw("Horizontal"),-Input.GetAxisRaw("Vertical"));
        position+=Vector2.ClampMagnitude(movement,1)*120*Mathf.Min(Time.deltaTime,0.25f);
        position.x=Mathf.Clamp(position.x,8,632);position.y=Mathf.Clamp(position.y,8,472);
        if(Vector2.Distance(position,new Vector2(520,240))<12) {score++;position=new Vector2(40,240);}
        if(Input.GetKeyDown(KeyCode.Escape)) {Save();Application.Quit();}
    }
    void Save() {PlayerPrefs.SetInt("nextos_training_score",score);PlayerPrefs.Save();}
    void OnApplicationPause(bool paused) {if(paused)Save();}
    void OnDestroy() {Save();if(pixel)Destroy(pixel);}
    void OnGUI() {
        GUI.matrix=Matrix4x4.Scale(new Vector3(Screen.width/640f,Screen.height/480f,1));
        GUI.color=new Color(0.08f,0.12f,0.18f);GUI.DrawTexture(new Rect(0,0,640,480),pixel);
        GUI.color=Color.yellow;GUI.DrawTexture(new Rect(512,232,16,16),pixel);
        GUI.color=Color.cyan;GUI.DrawTexture(new Rect(position.x-6,position.y-6,12,12),pixel);
        GUI.color=Color.white;GUI.Label(new Rect(10,10,500,30),"NextOS training / Treino - "+score);
    }
}
