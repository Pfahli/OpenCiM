#pragma once
#include <string>
#include <utility>
#include <vector>

#include "script/vm.h"

namespace regs::engine {

// Script-visible texture (gsTextureVariant_OGL). The pixels are loaded and uploaded by the display when first drawn.
class TextureObj : public script::Object {
 public:
  using Object::Object;
  std::u16string path;  // virtual path; empty for blank and render-target textures
  int width = 0, height = 0;
  uint32_t gl = 0;      // texture name once uploaded
  bool renderTarget = false;  // colour buffer of a gsTextureTarget_OGL (drawn into by a viewport)
  bool cube = false;          // DDS cube map (bound as GL_TEXTURE_CUBE_MAP)
  bool loadFailed = false;
};

// Texture cache by path (gsTextures_OGL): a missing or unreadable file gives textures/default.dds.
script::Ref<script::Object> getTexture(script::VM& vm, std::u16string path);

// Engine key codes (gsKey, docs/ui/input.md §1.4) used for mouse buttons and the wheel.
enum Key : int { kKeyControl = 9, kKeyAlt = 10, kKeyShift = 11, kMouseLeft = 143, kMouseMiddle = 144, kMouseRight = 145,
                 kWheelUp = 146, kWheelDown = 147, kKeyCount = 186 };
const char16_t* keyName(int key);

// GUI control (gsControl and subclasses): rectangle in parent coordinates, margins, visibility with timed blends,
// image, text, and the mouse/keyboard behaviour of docs/ui/input.md §3.
class ControlObj : public script::Object {
 public:
  using Object::Object;
  // geometry (+0xD8.., docs/ui/controls.md §2.2)
  float left = 0, top = 0, width = 0, height = 0;
  float marginL = 0, marginT = 0, marginR = 0, marginB = 0;
  float cropL = 0, cropT = 0, cropW = 1, cropH = 1;
  // image (+0x13C), created by background/hotBackground, setImage, setShader
  bool hasImage = false, hasMaterial = false;
  uint32_t background = 0x00FFFFFF, hotBackground = 0x00FFFFFF;
  script::Ref<script::Object> texture;  // material texture slot 0 (TextureObj)
  script::Ref<script::Object> slotTextures[7];  // material texture slots 1..7, read by the control's shader (masks)
  std::u16string shaderPath;            // material shader (setShader)
  // border (+0x140), created by setBorder
  bool hasBorder = false;
  uint32_t borderColor = 0, borderSides = 0;
  float borderWidth = 0;
  // text item (+0x144), created by text=, setFont, setTextShadow
  bool hasText = false;
  uint32_t textColor = 0xFFFFFFFF, shadowColor = 0x80000000;
  bool shadowPushed = false;  // the item's shadow colour stays 0 until setTextShadow or a visibility change
  float shadowDX = 0.001f, shadowDY = 0.001f;
  std::u16string fontName = u"fonts/verdana", fontExtra;
  float sizeX = 0.015f, sizeY = 0.015f, spacingX = 0, spacingY = 0;
  uint32_t textAlign = 9;
  std::u16string raw, display;  // text as set, and as drawn (formatted)
  bool formatEnabled = true, hasCodes = false;
  int maxTextLength = -1;
  std::vector<script::Value> args = std::vector<script::Value>(10);
  // visibility and blend (+0x1C8..+0x1D4)
  float visibility = 1.0f;
  bool blending = false;
  float blendTarget = 1.0f, blendRate = 1.0f;
  // input state (gsControl +0x186..+0x1C4)
  bool active = false, canActivate = true, clickThrough = false, clickThroughSelf = false;
  bool hot = false, dragging = false;
  float dragX = 0, dragY = 0;
  script::Ref<script::Object> activeChild, dragTarget;

  float desktopLeft() const;
  float desktopTop() const;
  bool clipped() const;  // entirely outside the owner (§3)
  void setRect(script::VM& vm, float l, float t, float w, float h);
  void setText(script::VM& vm, const std::u16string& s);  // §9.1
  void tick(script::VM& vm, float dt) override;           // §10.1

  ControlObj* parentControl() const;
  bool shown() const;  // visible and enabled, itself and every parent control (effective status & 3)
  bool firesMouseEvents() const;  // gsTextSelCtrl/gsEditCtrl handle the mouse themselves
  bool isTextSel() const;         // gsTextSelCtrl or gsEditCtrl
  bool isEdit() const;            // gsEditCtrl: text entry (docs/ui/input.md §7)
  float caretPhase = 0;           // gsEditCtrl caret blink, a triangle wave in −1..1 while active

  ControlObj* controlAt(float px, float py);  // slot 63
  void mouseOver(script::VM& vm);             // slots 68/69: parents first, then this
  void mouseOut(script::VM& vm);
  bool mouseMove(script::VM& vm, float x, float y, float dx, float dy, int buttons);  // slot 65
  void mouseDown(script::VM& vm, float x, float y, int key, int buttons);             // slot 66
  void mouseUp(script::VM& vm, float x, float y, int key, int buttons);               // slot 67
  // active-chain walk (slots 71-74): events on this level, then the active child
  void keyDown(script::VM& vm, int key, int mask);
  void keyUp(script::VM& vm, int key, int mask);
  void keyRepeat(script::VM& vm, int key, int mask);
  void keyChar(script::VM& vm, char32_t ch, int mask);
  bool shortCut(script::VM& vm, int key);
  void activate(script::VM& vm);              // slot 93: this and every parent
  void setActive(script::VM& vm, bool on);    // slot 90
  void bringToFront();
};

class Font;
struct TextParams;
uint32_t mulA(uint32_t argb, float v);  // alpha scaled by v, truncated (FUN_10074ef0)
// gsControl::formatText: % codes over argc arguments (controls.md §9.2); also the script expandPattern
std::u16string formatPattern(script::VM& vm, const std::u16string& pattern, const script::Value* args, int argc);
Font* controlFont(script::VM& vm, ControlObj* c);
// Text item parameters and position from alignment, margins and the measured display text (updateTextPos)
void controlTextPos(ControlObj* c, const Font& f, TextParams& p, float& x, float& y);

// gsDesktop: the input target of the GUI. Pointer position, hover, capture and the modifier mask (§3).
class DesktopObj : public ControlObj {
 public:
  using ControlObj::ControlObj;
  float mouseX = 0, mouseY = 0;
  script::Ref<script::Object> pointer, capture, hover;
  int mask = 0, lastDownKey = 0;
  bool shortcutMode = false;

  void inputMouseMove(script::VM& vm, float dx, float dy);  // gsInputObject slot 1
  void inputMousePos(script::VM& vm, float x, float y);     // slot 2
  void inputKeyDown(script::VM& vm, int key);               // slot 3
  void inputKeyUp(script::VM& vm, int key);                 // slot 6
  void inputFocus(script::VM& vm, bool on);                 // slot 7
  void resetMouseDown(script::VM& vm);
  void setPointer(script::VM& vm, script::Object* ctrl);
};

// gsWindow_OGL: frame statistics, input targets, then the update phase (children), then rendering and present.
class WindowObj : public script::Object {
 public:
  using Object::Object;
  double total = 0, sinceFps = 0;
  uint64_t frames = 0, framesSinceFps = 0;
  std::vector<std::pair<int, script::Ref<script::Object>>> targets;  // addInputTarget order
  script::Ref<script::Object> inputTarget, snapshot;
  bool appActive = true;
  float wheel = 0;

  void tick(script::VM& vm, float dt) override;
  void updateInputTarget(script::VM& vm);
};

void registerInputNatives(script::VM& vm);

}  // namespace regs::engine
