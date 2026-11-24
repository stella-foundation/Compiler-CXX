#[abi("C"), link_name("va_consume")]
pub fn consume(ap: *mut @va.list) -> i64 {
	return @va.arg(ap, i64);
}

#[abi("C"), link_name("va_mixed")]
pub fn va_mixed(marker: u64, ...) -> i32 {
	let mut ap: @va.list;
	@va.start(ap);
	let mut copied: @va.list;
	@va.copy(copied, ap);
	let first: i64 = consume(&ap);
	let again: i64 = @va.arg(copied, i64);
	@va.end(copied);
	let mut sum: i64 = first;
	for let mut i: usize = 0; i < 11; i += 1 { sum += consume(&ap); }
	let mut floats: f64 = 0.0;
	for let mut i: usize = 0; i < 12; i += 1 { floats += @va.arg(ap, f64); }
	let pointer: *u8 = @va.arg(ap, *u8);
	@va.end(ap);
	return marker == 99 && first == again && sum == 78 && floats == 78.0 && pointer[0] == 'x' ? 0 : 1;
}

#[abi("C")]
fn typed(_: u64, ...) -> i32 {
	let mut ap: @va.list;
	@va.start(ap);
	let a: i8 = @va.arg(ap, i8);
	let b: u16 = @va.arg(ap, u16);
	let c: f32 = @va.arg(ap, f32);
	let d: u128 = @va.arg(ap, u128);
	@va.end(ap);
	return a == -7 && b == 500 && c == 1.5 && d == ((1 cast u128) << 100) ? 0 : 2;
}

#[abi("C"), link_name("va_typed")]
pub fn va_typed() -> i32 {
	return typed(0 cast u64, -7 cast i8, 500 cast u16, 1.5 cast f32, (1 cast u128) << 100);
}
