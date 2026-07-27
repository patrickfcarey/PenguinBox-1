# Gesture → Button Mapping — concept

**Status:** concept / illustration only. Nothing here is built. It depends on
Quest Touch controller support (plan §10, backlog) which is itself unbuilt.
This doc shows *how it could work* end-to-end using the exact OpenXR + SDL
mechanisms the VR module already uses for head tracking and (planned) input.

Worked example throughout: **extend a controller sharply out to the side →
stiff-arm in ESPN NFL 2K5.** The same skeleton maps any punctuated motion to
any button.

---

## 1. The whole path in one picture

```
 OpenXR (WiVRn)                 xemu VR module (pfifo thread)            xemu input
 ──────────────                 ────────────────────────────            ──────────
 Touch controller               vr_gesture.c                            SDL virtual
 aim/grip POSE      ─xrLocateSpace─►  ring buffer of recent poses        gamepad
 (position+orient)                        │                              "Quest Touch (VR)"
                                          ▼                                   │
                                    recognizer state machine                  │
                                    (IDLE→ARMED→FIRED→COOLDOWN)               │
                                          │ on FIRE:                          ▼
                                          └── set synthetic button ──► xemu's normal
                                              in the virtual-pad state     controller stack
                                                                           (ports, rebind, game)
```

Every box except `vr_gesture.c` already exists or is already planned:
- `xrLocateSpace` on a per-controller space — same call we make for the head
  pose today, just on a different `XrSpace`.
- The SDL virtual gamepad — the §10 controller bridge (`SDL_AttachVirtualJoystick`);
  xemu treats it as a real pad, so **no new UI**.
- The pfifo→UI handoff — the qatomic mailbox pattern we already use for the
  recenter flag.

So the *only* new code is the recognizer: a small state machine that reads
poses and sets one button. ~40 lines.

## 2. The one honest constraint: no torso

A Quest tracks **head + two controllers**. There is no chest/hip tracker, so
"out to the side" cannot mean "lateral to your torso" — we don't know where
your torso faces. The only body anchor is the **head**.

So we define the gesture in a **head-relative frame**: take the head's yaw
(we already compute it for recenter), build right/up/forward vectors from it,
and express the controller's position relative to the head in that frame.

```c
/* head-relative controller position (metres): +x right, +y up, +z back */
XrVector3f d = { ctrl.pos.x - head.pos.x,
                 ctrl.pos.y - head.pos.y,
                 ctrl.pos.z - head.pos.z };
float cy = cosf(head_yaw), sy = sinf(head_yaw);
float lateral =  d.x * cy - d.z * sy;   /* right(+) / left(-) of head  */
float vertical = d.y;                    /* above(+) / below(-) head    */
```

`lateral` is the number that matters for a stiff-arm: how far the controller
is to the side of your head. Consequence to accept: it's accurate when you
face roughly forward, and degrades if you're turned sideways — fine for a
seated football game, a known limit to document, not a bug to chase.

## 3. Why a *motion*, not a *pose*

If the trigger were "arm is extended" (`lateral > 0.4 m`), you'd stiff-arm
every time you rested your arm out to the side — constant misfires. The
gesture must be **punctuated**: extend *fast*. That also feels right — a
stiff-arm is a shove, not a posture.

So we gate on **lateral velocity** past a threshold, from a near-body start:

```c
/* per frame, dt from predictedDisplayTime deltas */
float v_lateral = (lateral - prev_lateral) / dt;   /* m/s */
```

Fire when the arm was tucked in, then shot outward fast:

```
ARMED   when lateral < 0.15 m           (arm near the body — "loaded")
FIRE    when v_lateral > 2.0 m/s        (fast outward thrust)
        AND lateral crossed > 0.35 m    (actually reached out, not a twitch)
```

## 4. The state machine (the entire new module)

```
        ┌─────────────────────────────────────────────────────────┐
        ▼                                                         │
   ┌─────────┐  lateral<0.15   ┌────────┐  v>2.0 & reached>0.35  ┌──────┐
   │  IDLE   │───────────────► │ ARMED  │──────────────────────► │ FIRE │
   └─────────┘                 └────────┘                        └──────┘
        ▲                          │ (timeout 1s, no thrust)         │ set button
        │                          ▼                                 │ true 1 frame
        │                      back to IDLE                          ▼
        │                                                       ┌──────────┐
        └───────────────────────────────────────────────────── │ COOLDOWN │
                              (250 ms; button released)         └──────────┘
```

- **ARMED** prevents firing from a static extended arm — you must first bring
  the controller in, then thrust out.
- **FIRE** sets the virtual-pad button true for one frame (or a few, to satisfy
  the game's input sampling), then...
- **COOLDOWN** blocks retriggers for ~250 ms so one thrust = one stiff-arm.

```c
switch (g->state) {
case IDLE:
    if (lateral < 0.15f) g->state = ARMED, g->armed_time = now;
    break;
case ARMED:
    if (v_lateral > 2.0f && lateral > 0.35f) {
        vpad_set_button(STIFF_ARM_BTN, true);   /* → virtual gamepad */
        g->state = FIRE; g->fire_time = now;
    } else if (now - g->armed_time > 1.0f) {
        g->state = IDLE;                          /* never thrust — re-arm */
    }
    break;
case FIRE:
    if (now - g->fire_time > 0.05f) {             /* held ~50 ms */
        vpad_set_button(STIFF_ARM_BTN, false);
        g->state = COOLDOWN; g->cool_time = now;
    }
    break;
case COOLDOWN:
    if (now - g->cool_time > 0.25f) g->state = IDLE;
    break;
}
g->prev_lateral = lateral;
```

That's the whole thing. Right controller → right stiff-arm, mirror `lateral`
sign for the left.

## 5. Config surface (matches the existing [vr] style)

```
[vr]
gesture_enable          = true
gesture_stiffarm_button = "..."     # which pad button the game reads
gesture_lateral_thresh  = 0.35      # metres the arm must reach
gesture_velocity_thresh = 2.0       # m/s outward to count as a thrust
```

Per-game bindings would eventually live in the profile DB (like camera
addresses), so "arm-out = stiff-arm" ships as ESPN NFL 2K5's profile rather
than a global.

## 6. Honest verdict (why this is a "could", not a "should… yet")

- **Thematically perfect.** Physically shoving your arm out to stiff-arm a
  defender is exactly the VR-makes-it-magic moment.
- **Latency is the catch.** move → recognize → inject is inherently slower
  than a button, and football wants the stiff-arm frame-perfect before
  contact. This will feel like a delightful party trick before it feels
  competitive.
- **Best-fit targets** are gesture-IS-the-fantasy, timing-forgiving moves:
  bat swing, jab, golf swing, a basketball shot. Stiff-arm sits on the line —
  maximally evocative, marginally practical.
- **Cheap to try.** Because it rides the controller bridge and the virtual
  pad, prototyping the recognizer is an afternoon once controllers exist —
  low enough cost that "is it fun?" is worth answering empirically.

Sequencing: after Tier-3 (OFP) proves out, after §10 controller input lands.
Then this is one small, self-contained, high-delight experiment.
