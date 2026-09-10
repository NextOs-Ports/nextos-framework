// SPDX-License-Identifier: GPL-3.0-only
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEditor.Build.Reporting;
using UnityEngine;
public static class TrainingBuild {
    public static void Android() {
        var scene=EditorSceneManager.NewScene(NewSceneSetup.EmptyScene,NewSceneMode.Single);
        new GameObject("NextOS Training").AddComponent<Training>();
        EditorSceneManager.SaveScene(scene,"Assets/Training.unity");
        PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android,"org.nextos.training.unity");
        PlayerSettings.SetScriptingBackend(BuildTargetGroup.Android,ScriptingImplementation.IL2CPP);
        PlayerSettings.Android.targetArchitectures=AndroidArchitecture.ARM64;
        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android,false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android,new[]{UnityEngine.Rendering.GraphicsDeviceType.OpenGLES2});
        System.IO.Directory.CreateDirectory("Build");
        var report=BuildPipeline.BuildPlayer(new[]{"Assets/Training.unity"},"Build/training.apk",BuildTarget.Android,BuildOptions.Development);
        if(report.summary.result!=BuildResult.Succeeded) throw new System.Exception("Android training build failed");
    }
}
