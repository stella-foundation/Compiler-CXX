fn invalid_range_order(x: u32) -> u1 {
	return x[0:1];
}

fn invalid_range_width(x: u32) -> u1 {
	return x[32:32];
}

fn invalid_range_dynamic(x: u32, high: u8) -> u1 {
	return x[high:high];
}

fn invalid_range_negative(x: u32) -> u1 {
	return x[0:-1];
}

fn invalid_range_type(x: f32) -> u1 {
	return x[0:0];
}
