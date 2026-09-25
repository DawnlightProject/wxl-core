Thick ground fog that pools in low places, thins with height and drifts with the wind.

The world's depth is sampled on the GPU to rebuild where every pixel sits, and a short raymarch
through an animated noise field decides how much fog stands between it and the camera. Its colour
is the zone's own fog colour, so it follows the time of day.

Needs a graphics card that can read depth as a texture (INTZ) and multisampling turned off; without
either it stays inert.
