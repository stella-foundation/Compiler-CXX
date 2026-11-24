fn invalid_wrap_float(x: f32, y: f32) -> f32 {
	return x +% y;
}

fn invalid_sat_mismatch(x: u8, y: u16) -> u8 {
	return x +| y;
}
