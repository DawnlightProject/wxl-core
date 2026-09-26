Shadows for a world lit by lamps and torches. Every lamp that matters gets a real shadow map: its
post, the fence, the walls and every passer-by throw soft shadows on the ground and into the fog.
A lamp's still surroundings are drawn once and kept; only the moving bodies are drawn again, so a
street of lamps stays cheap. Lamps without a map still let bodies cut their light, with soft capsule
shadows traced along the beam. A torch in a hand throws its carrier's shadow on everything around,
never on the carrier.

The sun and the moon get what the engine lacks: hills and mountains cast on the land, shadows grow
long and soft at dusk, and the moon casts at night. Small contact shadows ground every foot and
stone.

Every shadow is filtered so it never shows a grain, a stripe or a crawl as the camera moves: what you
see depends on the world, not on where you look from. Other extensions read the same shadows, for
surfaces and for the air, through one published API.

It runs as compute on the Vulkan device behind DXVK and needs Graphics Extend. Without DXVK, or
without it, everything is simply unshadowed.
