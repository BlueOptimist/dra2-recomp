/* MSVC's x87 math helpers, as the game calls them after analysis/crt_signatures.tsv gave them signatures.
 * On the Xbox they take their operands on the FPU stack (ST0, ST1); the decompiled callers now pass them as ordinary
 * arguments, so these are plain C. float10 is the decompiler's x87 type; the build spells it float (ghidra_types.h). */
double acos(double x);
double asin(double x);
double atan2(double y, double x);
double pow(double x, double y);

/* _ftol: truncate toward zero to a 64-bit integer (returned in EDX:EAX) */
long long CRT_ftol(float value) { return (long long)value; }

float CRT_CIacos(float x) { return (float)acos(x); }
float CRT_CIasin(float x) { return (float)asin(x); }
float CRT_CIatan2(float y, float x) { return (float)atan2(y, x); }
float CRT_CIpow(float x, float y) { return (float)pow(x, y); }
