/* Controller input ids, decoded from PadController::QueryButton (0x0996b0) and QueryAction (0x09a030).
 *
 * A Character asks its controller (AIController or PadController at +0xe8) about an action:
 *   QueryButton(action, holdTime, buffered)   ids 0x00-0x12, answered from the pad or the input history
 *   QueryAction(action, holdTime, held)       ids 0x14-0x1d; ids below 0x13 are passed to QueryButton
 *
 * "Stick" directions go through the arena's camera side (+0x90), so left/right/up/down are on screen.
 * "Forward"/"back" are relative to the direction the character faces (+0x98..+0xa0).
 * "Buffered" means the press appears anywhere in the recent input history (+0x30 list).
 * Names in parentheses are inferences from how Character uses the id; the rest is the input pattern.
 */
#ifndef DRA2_INPUT_ACTIONS_H
#define DRA2_INPUT_ACTIONS_H

/* Logical buttons: per-player tables of raw pad button indices, filled by SetControlScheme (0x095200). */
enum PadBinding {
    BIND_JUMP     = 0, /* table 0x2f6164; Jump and DownJump read it */
    BIND_BUTTON2  = 1, /* table 0x2f6174; with block held it is BlockPlusButton2 (grab?) */
    BIND_BUTTON3  = 2, /* table 0x2f6184; tap and hold variants (main attack?) */
    BIND_BLOCK    = 3, /* table 0x2f6194; always the left trigger */
    BIND_BUTTON4  = 4, /* table 0x2f61a4; adds charge while the character is in animation state 0xf */
    BIND_DIGIVOLVE = 5 /* table 0x2f61b4; always the right trigger */
};

/* Raw Xbox gamepad analog-button indices stored in those tables. */
enum PadButton {
    PAD_A = 0, PAD_B = 1, PAD_X = 2, PAD_Y = 3, PAD_BLACK = 4, PAD_WHITE = 5,
    PAD_LEFT_TRIGGER = 6, PAD_RIGHT_TRIGGER = 7
};

/* Default layouts written by SetControlScheme(scheme, player):
 *                    JUMP  BUTTON2  BUTTON3  BUTTON4  BLOCK  DIGIVOLVE
 *   scheme 0 (A)     A     X        B        Y        LT     RT     (when global 0x2f5638 == 1)
 *   scheme 0 (B)     Y     A        X        B        LT     RT     (otherwise)
 *   scheme 1         A     X        Y        B        LT     RT
 */

enum InputAction {
    ACT_BUTTON3_WITH_BLOCK = 0x00, /* BUTTON3 and BLOCK both down */
    ACT_STICK_FORWARD      = 0x01, /* buffered: stick toward the facing direction */
    ACT_STICK_BACK         = 0x02, /* buffered: stick away from the facing direction */
    ACT_WIGGLE_HORIZONTAL  = 0x03, /* history shows the stick going left-right repeatedly */
    ACT_WIGGLE_VERTICAL    = 0x04, /* history shows the stick going up-down repeatedly */
    /* 0x05-0x08: one-direction variants of the wiggle counters */
    ACT_STICK_UP           = 0x09, /* buffered: stick within 45 degrees of up */
    ACT_STICK_DOWN         = 0x0a, /* buffered: stick within 45 degrees of down */
    ACT_BUTTON4_PRESSED    = 0x0b, /* buffered BUTTON4 press */
    /* 0x0c: buffered press of a sixth table (getter 0x099170), only while +0xa4 == 2 */
    ACT_BUTTON3_PRESSED    = 0x0d, /* buffered BUTTON3 press */
    ACT_BUTTON3_HELD       = 0x0e, /* BUTTON3 down for at least holdTime */
    /* 0x0f-0x12: further held-time queries (0x0f reads the hold timer at +0x34) */

    ACT_CROUCH             = 0x14, /* stick down past 0.5; Character sets its crouch flag (+0x267) */
    ACT_BLOCK              = 0x15, /* BLOCK down; Character sets its block flag (+0x269) */
    ACT_BLOCK_PLUS_BUTTON2 = 0x16, /* BLOCK and BUTTON2 together (grab?) */
    ACT_DIGIVOLVE          = 0x17, /* DIGIVOLVE pressed; Character then checks "nodigivolve" (0x0e2aa0) */
    ACT_MOVE_RIGHT         = 0x18, /* stick right past 0.25; ReadMoveInput direction +1 */
    ACT_MOVE_LEFT          = 0x19, /* stick left past 0.25; ReadMoveInput direction -1 */
    ACT_DOWN_JUMP          = 0x1a, /* stick down past 0.25 and JUMP pressed; TryDownJump */
    ACT_JUMP               = 0x1b, /* JUMP pressed (or held when asked), or stick up if the "stick jumps" option is on */
    ACT_SUPER_JUMP         = 0x1c, /* stick flicked up past 0.25 while the flick timer runs; only with superJump */
    ACT_MASH               = 0x1d  /* a stick wiggle (ids 3/4) or another mash check (0x099ee0, input history);
                                      shortens stun/woozy and grab-escape timers */
};

#endif
