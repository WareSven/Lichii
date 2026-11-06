# Nintendo Switch 2 Pro Controller (USB)

Connect the controller by USB, then open **Emulation > Configure > Controls**.
Select **Nintendo Switch 2 Pro Controller** as the input device and keep the
emulated controller type set to **Pro Controller**.

To use GL or GR in addition to an existing button:

1. Right-click the command you want to duplicate, such as A or B.
2. Choose **Add alternate button** (French: **Ajouter un bouton secondaire**).
3. Press GL or GR. Both assignments appear separated by `/`.
4. Apply the settings, or save them to an input profile.

The original button remains active. Holding both sources keeps the command pressed
until both are released. Each command supports one alternate source. Right-click
again to change or remove it. A normal click replaces the primary source while
keeping its alternate; **Clear** removes both.

GL, GR and C are physical inputs that can be assigned to existing emulated commands.
This does not add new GL/GR buttons to the emulated controller protocol.
Native Bluetooth support is outside this change.

## Implementation and upstream

USB support is ported from [Eglantine PR #573](https://github.com/Djihads80/Eglantine/pull/573),
commit `a7fafb8ad224a6337465b9a2eb84215ba60e8d30`. Windows builds with
`ENABLE_LIBUSB=ON` and `YUZU_USE_BUNDLED_SDL3=ON` build SDL 3.4.20 with libusb.
The parent libusb dependency uses `BUILD_LOCAL_INTERFACE` so SDL's build-tree
exports do not require exporting Lichii's internal `usb` target.

Button names follow SDL's actual bindings for Nintendo VID `057e`, PID `2069`:
`LEFT_PADDLE1` is GL, `RIGHT_PADDLE1` is GR and `MISC2` is C.
Alternate inputs are stored in the existing command parameter package as a
serialized `secondary` package, so input profiles retain both sources.

Regression tests cover virtual SDL input, remapped button indices, distinct
controller ports, disconnection, overlapping inputs, and saved bindings. Physical
controller operation still needs an on-device check with the resulting build.
