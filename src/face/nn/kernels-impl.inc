// The SIMD convolutions (kernels.hpp), written once and built twice: by
// kernels-sse2.cpp and kernels-avx2.cpp, each with its own 8-float type
// `V` and these functions on it, inside an unnamed namespace:
//   V vload(const float *), void vstore(float *, V), V vset(float),
//   V vfma(V a, V b, V c) (a * b + c), V vadd(V, V), V vmax(V, V),
//   V vmin(V, V), V vprelu(V x, V slope) (x if x >= 0, else x * slope)
// and MR_TWO / MR_ONE: how many pixels a tile takes with two panels or
// one. No standard library here (see kernels.hpp).

static inline int imin(int a, int b)
{
	return a < b ? a : b;
}

static inline int imax(int a, int b)
{
	return a > b ? a : b;
}

// What happens to a finished value: + residual, PReLU, clamp. `n` is how
// many of the 8 channels are real (the last panel can be short).
static inline void finish(V v, const Conv &j, int ch, int n, float *out, const float *residual)
{
	if (n == PANEL) {
		if (residual)
			v = vadd(v, vload(residual + ch));
		if (j.slope)
			v = vprelu(v, vload(j.slope + ch));
		v = vmin(vmax(v, vset(j.lo)), vset(j.hi));
		vstore(out + ch, v);
		return;
	}
	float tmp[PANEL];
	vstore(tmp, v);
	for (int i = 0; i < n; i++) {
		float x = tmp[i];
		if (residual)
			x += residual[ch + i];
		if (j.slope && !(x >= 0))
			x *= j.slope[ch + i];
		x = x < j.lo ? j.lo : x;
		x = x > j.hi ? j.hi : x;
		out[ch + i] = x;
	}
}

// MR output pixels by NP panels (8 x NP channels from `panel`). `taps`
// is the input pixel each tap of each pixel reads: taps[t * MR + r].
//
// Every sum is a variable of its own, written out row by row, so it stays
// in a register whatever the compiler thinks of unrolling loops.
#define DB_ROWS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11)
template<int MR, int NP>
static void tile(const Conv &j, const float *const *taps, int tap_count, int panel, float *const *outs,
		 const float *const *residuals)
{
	static_assert(MR >= 1 && MR <= 12 && NP >= 1 && NP <= 2, "tile size");
	const int in_c = j.in_c;
	const size_t panel_size = (size_t)tap_count * in_c * PANEL;
	const float *w = j.weights + (size_t)panel * panel_size;
	const V b0 = vload(j.bias + panel * PANEL);
	const V b1 = NP > 1 ? vload(j.bias + (panel + 1) * PANEL) : b0;
#define DB_SUMS(r) V c##r##_0 = b0, c##r##_1 = b1;
	DB_SUMS(0) DB_SUMS(1) DB_SUMS(2) DB_SUMS(3) DB_SUMS(4) DB_SUMS(5)
	DB_SUMS(6) DB_SUMS(7) DB_SUMS(8) DB_SUMS(9) DB_SUMS(10) DB_SUMS(11)
#undef DB_SUMS
	for (int t = 0; t < tap_count; t++) {
		const float *const *at = taps + (size_t)t * MR;
#define DB_PTR(r) const float *p##r = at[MR > r ? r : 0];
		DB_ROWS(DB_PTR)
#undef DB_PTR
		const float *wt = w + (size_t)t * in_c * PANEL;
		for (int ci = 0; ci < in_c; ci++, wt += PANEL) {
			const V w0 = vload(wt);
			const V w1 = NP > 1 ? vload(wt + panel_size) : w0;
#define DB_ROW(r)                                          \
	if constexpr (MR > r) {                            \
		const V a = vset(p##r[ci]);                \
		c##r##_0 = vfma(a, w0, c##r##_0);          \
		if constexpr (NP > 1)                      \
			c##r##_1 = vfma(a, w1, c##r##_1);  \
	}
			DB_ROWS(DB_ROW)
#undef DB_ROW
		}
	}
	const int ch0 = panel * PANEL, ch1 = ch0 + PANEL;
	const int n0 = imin(PANEL, j.out_c - ch0), n1 = imin(PANEL, j.out_c - ch1);
#define DB_OUT(r)                                                                                     \
	if constexpr (MR > r) {                                                                       \
		const float *res = residuals ? residuals[r] : nullptr;                                \
		finish(c##r##_0, j, ch0, n0, outs[r], res);                                          \
		if constexpr (NP > 1)                                                                 \
			finish(c##r##_1, j, ch1, n1, outs[r], res);                                  \
	}
	DB_ROWS(DB_OUT)
#undef DB_OUT
}
#undef DB_ROWS

// All panels for MR pixels: two at a time, then one.
template<int MR>
static void tiles(const Conv &j, const float *const *taps, int tap_count, float *const *outs,
		  const float *const *residuals, int panels, bool two)
{
	int p = 0;
	if (two)
		for (; p + 2 <= panels; p += 2)
			tile<MR, 2>(j, taps, tap_count, p, outs, residuals);
	for (; p < panels; p++)
		tile<MR, 1>(j, taps, tap_count, p, outs, residuals);
}

// MR output pixels in a row (row after row, so a tile can run on to the
// next line), from pixel `first`.
template<int MR>
static void run_tile(const Conv &j, int first, int panels, bool two)
{
	const int tap_count = j.kh * j.kw;
	const float *taps[MOST_TAPS * MR];
	float *outs[MR];
	const float *residuals[MR];
	const bool pointwise = tap_count == 1 && j.stride_h == 1 && j.stride_w == 1 && j.pad_top == 0 && j.pad_left == 0 &&
			       j.in_w == j.out_w && j.in_h == j.out_h;
	for (int r = 0; r < MR; r++) {
		const int px = first + r;
		outs[r] = j.out + (size_t)px * j.out_c;
		residuals[r] = j.residual ? j.residual + (size_t)px * j.out_c : nullptr;
		if (pointwise) {
			// A 1 x 1 filter: the input pixel is the output one.
			taps[r] = j.in + (size_t)px * j.in_c;
			continue;
		}
		const int oy = px / j.out_w, ox = px % j.out_w;
		int t = 0;
		for (int ky = 0; ky < j.kh; ky++) {
			const int iy = oy * j.stride_h - j.pad_top + ky * j.dil_h;
			for (int kx = 0; kx < j.kw; kx++, t++) {
				const int ix = ox * j.stride_w - j.pad_left + kx * j.dil_w;
				const bool inside = iy >= 0 && iy < j.in_h && ix >= 0 && ix < j.in_w;
				taps[t * MR + r] = inside ? j.in + ((size_t)iy * j.in_w + ix) * j.in_c : j.zeros;
			}
		}
	}
	tiles<MR>(j, taps, tap_count, outs, j.residual ? residuals : nullptr, panels, two);
}

static void conv(const Conv &j)
{
	const int first = j.row_from * j.out_w, last = j.row_to * j.out_w;
	const int panels = (j.out_c + PANEL - 1) / PANEL;
	const bool two = panels >= 2;
	int px = first;
	if (two)
		for (; px + MR_TWO <= last; px += MR_TWO)
			run_tile<MR_TWO>(j, px, panels, true);
	else
		for (; px + MR_ONE <= last; px += MR_ONE)
			run_tile<MR_ONE>(j, px, panels, false);
	// The few left over, one at a time.
	for (; px < last; px++)
		run_tile<1>(j, px, panels, two);
}

// One output pixel's channels [c0, c0 + 8), every tap checked.
static inline V dw_edge(const Depthwise &j, int oy, int ox, int c0)
{
	V acc = vload(j.bias + c0);
	for (int ky = 0; ky < j.kh; ky++) {
		const int iy = oy * j.stride_h - j.pad_top + ky * j.dil_h;
		if (iy < 0 || iy >= j.in_h)
			continue;
		for (int kx = 0; kx < j.kw; kx++) {
			const int ix = ox * j.stride_w - j.pad_left + kx * j.dil_w;
			if (ix < 0 || ix >= j.in_w)
				continue;
			acc = vfma(vload(j.in + ((size_t)iy * j.in_w + ix) * j.c + c0),
				   vload(j.weights + ((size_t)ky * j.kw + kx) * j.c + c0), acc);
		}
	}
	return acc;
}

static inline void dw_finish(const Depthwise &j, V v, float *out, int c0)
{
	if (j.slope)
		v = vprelu(v, vload(j.slope + c0));
	v = vmin(vmax(v, vset(j.lo)), vset(j.hi));
	vstore(out + c0, v);
}

// The channels past the last whole 8, one at a time.
static void dw_tail(const Depthwise &j, int oy, int ox, int c0, float *out)
{
	for (int c = c0; c < j.c; c++) {
		float acc = j.bias[c];
		for (int ky = 0; ky < j.kh; ky++) {
			const int iy = oy * j.stride_h - j.pad_top + ky * j.dil_h;
			if (iy < 0 || iy >= j.in_h)
				continue;
			for (int kx = 0; kx < j.kw; kx++) {
				const int ix = ox * j.stride_w - j.pad_left + kx * j.dil_w;
				if (ix < 0 || ix >= j.in_w)
					continue;
				acc += j.in[((size_t)iy * j.in_w + ix) * j.c + c] * j.weights[((size_t)ky * j.kw + kx) * j.c + c];
			}
		}
		if (j.slope && !(acc >= 0))
			acc *= j.slope[c];
		acc = acc < j.lo ? j.lo : acc;
		acc = acc > j.hi ? j.hi : acc;
		out[c] = acc;
	}
}

static void depthwise(const Depthwise &j)
{
	const int whole = j.c / PANEL * PANEL;
	const bool three = j.kh == 3 && j.kw == 3 && j.dil_h == 1 && j.dil_w == 1;
	// The output pixels whose taps are all inside the picture.
	const int x_lo = imin(j.out_w, imax(0, (j.pad_left + j.stride_w - 1) / j.stride_w));
	int x_hi = j.out_w;
	while (x_hi > x_lo && (x_hi - 1) * j.stride_w - j.pad_left + (j.kw - 1) * j.dil_w >= j.in_w)
		x_hi--;
	for (int oy = j.row_from; oy < j.row_to; oy++) {
		const int iy0 = oy * j.stride_h - j.pad_top;
		const bool rows_inside = iy0 >= 0 && iy0 + (j.kh - 1) * j.dil_h < j.in_h;
		float *out_row = j.out + (size_t)oy * j.out_w * j.c;
		for (int c0 = 0; c0 < whole; c0 += PANEL) {
			if (rows_inside && three && x_hi > x_lo) {
				// The usual case, the filter's 9 weights held throughout.
				V w[9];
				for (int t = 0; t < 9; t++)
					w[t] = vload(j.weights + (size_t)t * j.c + c0);
				const V b = vload(j.bias + c0);
				const size_t line = (size_t)j.in_w * j.c;
				for (int ox = 0; ox < x_lo; ox++)
					dw_finish(j, dw_edge(j, oy, ox, c0), out_row + (size_t)ox * j.c, c0);
				for (int ox = x_lo; ox < x_hi; ox++) {
					const float *p = j.in + ((size_t)iy0 * j.in_w + (ox * j.stride_w - j.pad_left)) * j.c + c0;
					V acc = b;
					acc = vfma(vload(p), w[0], acc);
					acc = vfma(vload(p + j.c), w[1], acc);
					acc = vfma(vload(p + 2 * j.c), w[2], acc);
					p += line;
					acc = vfma(vload(p), w[3], acc);
					acc = vfma(vload(p + j.c), w[4], acc);
					acc = vfma(vload(p + 2 * j.c), w[5], acc);
					p += line;
					acc = vfma(vload(p), w[6], acc);
					acc = vfma(vload(p + j.c), w[7], acc);
					acc = vfma(vload(p + 2 * j.c), w[8], acc);
					dw_finish(j, acc, out_row + (size_t)ox * j.c, c0);
				}
				for (int ox = x_hi; ox < j.out_w; ox++)
					dw_finish(j, dw_edge(j, oy, ox, c0), out_row + (size_t)ox * j.c, c0);
			} else {
				for (int ox = 0; ox < j.out_w; ox++)
					dw_finish(j, dw_edge(j, oy, ox, c0), out_row + (size_t)ox * j.c, c0);
			}
		}
		if (whole < j.c)
			for (int ox = 0; ox < j.out_w; ox++)
				dw_tail(j, oy, ox, whole, out_row + (size_t)ox * j.c);
	}
}
