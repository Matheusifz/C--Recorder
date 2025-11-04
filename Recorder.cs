using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.InputSystem;

[Serializable]
public class InputFrame
{
    public float t;                // time since recording start
    public Vector2 move;           // keyboard movement vector
    public Vector2 look;           // mouse delta (rawish)
    public bool fire;              // LMB
    public bool altFire;           // RMB
    public List<string> keysDown;  // any extra keys pressed this frame
}

[Serializable]
public class InputRecording
{
    public float duration;
    public List<InputFrame> frames = new List<InputFrame>();
}

public class Recorder : MonoBehaviour
{
    [Header("Bindings")]
    public InputActionAsset actions;

    [Header("Config")]
    public string saveFileName = "recording.json";
    public bool recordOnPlay = false;
    public bool captureExtraKeys = true;
    public Key[] extraKeys = new Key[] {
        Key.Space, Key.LeftShift, Key.LeftCtrl, Key.E, Key.Q, Key.R, Key.F
    };

    InputAction move;
    InputAction look;
    InputAction fire;
    InputAction altFire;

    InputRecording rec;
    bool isRecording;
    float t0;

    void Awake()
    {
        if (actions == null)
        {
            Debug.LogError("Assign an InputActionAsset.");
            enabled = false; return;
        }

        // Assuming action maps named "Player"
        var map = actions.FindActionMap("Player", true);
        move = map.FindAction("move", true);
        look = map.FindAction("look", true);
        fire = map.FindAction("fire", true);
        altFire = map.FindAction("altFire", true);
        map.Enable();
    }

    void Start()
    {
        if (recordOnPlay) StartRecording();
    }

    public void StartRecording()
    {
        rec = new InputRecording { frames = new List<InputFrame>() };
        t0 = Time.realtimeSinceStartup;
        isRecording = true;
        Debug.Log("[Recorder] Started.");
    }

    public void StopRecordingAndSave()
    {
        if (!isRecording) return;
        isRecording = false;
        rec.duration = Time.realtimeSinceStartup - t0;

        var path = Path.Combine(Application.persistentDataPath, saveFileName);
        File.WriteAllText(path, JsonUtility.ToJson(rec, prettyPrint: true));
        Debug.Log($"[Recorder] Saved to {path}");
    }

    void Update()
    {
        if (!isRecording) return;

        var frame = new InputFrame
        {
            t = Time.realtimeSinceStartup - t0,
            move = move.ReadValue<Vector2>(),
            look = look.ReadValue<Vector2>(),
            fire = fire.IsPressed(),
            altFire = altFire.IsPressed(),
            keysDown = new List<string>()
        };

        if (captureExtraKeys)
        {
            var kb = Keyboard.current;
            if (kb != null)
            {
                foreach (var k in extraKeys)
                {
                    var keyCtrl = kb[k];
                    if (keyCtrl != null && keyCtrl.isPressed)
                        frame.keysDown.Add(k.ToString());
                }
            }
        }

        rec.frames.Add(frame);
    }

    // Simple manual control for testing
    void OnGUI()
    {
        const int w = 180, h = 28, pad = 10;
        if (GUI.Button(new Rect(pad, pad, w, h), isRecording ? "Stop & Save" : "Start Recording"))
        {
            if (isRecording) StopRecordingAndSave(); else StartRecording();
        }
        GUI.Label(new Rect(pad, pad + h + 6, 400, 24),
            isRecording ? $"Recording… frames: {rec.frames.Count}" : "Idle");
    }
}
