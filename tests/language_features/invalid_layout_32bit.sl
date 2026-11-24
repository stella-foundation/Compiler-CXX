#[layout(16, 8)]
struct PointerHeader {
	value: *u8,
	tag: u8,
}
