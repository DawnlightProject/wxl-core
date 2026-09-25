The plumbing every graphics extension needs, done once and shared.

It gives other extensions the world's depth as a texture (INTZ), the G-buffer normals the engine's
rewritten materials write, and an HDR copy of the world. It creates them only on the frames an
extension asks for them, and gives the same textures to all of them, so two effects never fight over
the depth buffer. Passes are scheduled in a fixed order after the world is drawn, and each one's GPU
time is measured. The toolbox also covers render targets that survive resizes and device resets, DDS
and BLS files, cached HLSL compilation and a small shader library.

It draws nothing on its own. Install it alongside the extensions that use it.
