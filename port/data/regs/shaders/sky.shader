// ReGS sky (engine/sky.cpp addSkyItems): the port's own sky dome, drawn where the game's own sky is switched off
// (engine.script: $sky.visible = false). The game's camera never looked above the horizon; the port's tilt and far
// view do. Blue by day, grey under rain or snow, a sunset toward a low sun, dark with stars and the moon at night;
// sky.cpp works out the colours from the sun and the scene's atmosphere (the weather mod) and passes them in custom0-5
// (see sky.fragment).
this.matMask = 0;
this.meshMask = 0;
this.envMask = 0;

event this.onProgram::($matFlags, $meshFlags, $envFlags) {
  this.attributes = POSITION;
  this.textures = 0;
  this.parameters = SHADER_CUSTOM0 | SHADER_CUSTOM1 | SHADER_CUSTOM2 | SHADER_CUSTOM3 | SHADER_CUSTOM4 | SHADER_CUSTOM5 | SHADER_CUSTOM6;

  this.vertexComponent = "sky.vertex";
  this.fragmentComponent = "sky.fragment";
}
