#[layout(8, 8)]
struct Cell<T> {
	value: T,
}

pub fn cell_load(x: *Cell<u32>) -> u32 {
	return x.value;
}

pub fn cell_load64(x: *Cell<u64>) -> u64 {
	return x.value;
}
