/* CI check for the headers that ship in the repository: they compile on their own as 32-bit Windows C, and the
 * resource format structs keep the sizes the game data was verified against (same checks as compile_check.py). */
#include "ghidra_types.h"
#include "dra2_formats.h"
#include "input_actions.h"

_Static_assert(sizeof(Bone) == 160, "Bone");
_Static_assert(sizeof(SkinnedVertex) == 60, "SkinnedVertex");
_Static_assert(sizeof(StaticVertex) == 44, "StaticVertex");
_Static_assert(sizeof(ShaderLayer) == 120, "ShaderLayer");
_Static_assert(sizeof(ResTocEntry) == 20, "ResTocEntry");
_Static_assert(sizeof(MeshDescriptor) == 44, "MeshDescriptor");
_Static_assert(sizeof(XboxWaveFormat) == 20, "XboxWaveFormat");
_Static_assert(sizeof(ResIndexEntry) == 12, "ResIndexEntry");
_Static_assert(ACT_MASH == 0x1d, "input action ids");
