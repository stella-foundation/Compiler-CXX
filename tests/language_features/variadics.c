#include <stdint.h>
#include <stdarg.h>
extern int va_mixed(uint64_t, ...);
extern int va_typed(void);
extern int64_t va_consume(va_list *);
static int from_c(int marker, ...) {
	va_list ap;
	va_start(ap, marker);
	int64_t first = va_consume(&ap);
	int64_t next = va_arg(ap, int64_t);
	va_end(ap);
	return first != 13 || next != 17;
}
int main(void) {
	return va_mixed(99, (int64_t)1, (int64_t)2, (int64_t)3, (int64_t)4,
		(int64_t)5, (int64_t)6, (int64_t)7, (int64_t)8, (int64_t)9,
		(int64_t)10, (int64_t)11, (int64_t)12,
		1., 2., 3., 4., 5., 6., 7., 8., 9., 10., 11., 12., "x") || va_typed() || from_c(0, (int64_t)13, (int64_t)17);
}
