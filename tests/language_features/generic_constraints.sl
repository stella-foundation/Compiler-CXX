fn unsigned_identity<T: u>(value: T) -> T {
	return value;
}

fn signed_identity<T: i>(value: T) -> T {
	return value;
}

fn test_unsigned_constraint(u: u8) -> u8 {
	return unsigned_identity(u);
}

fn test_signed_constraint(value: i8) -> i8 {
	let i: i8 = signed_identity(value);
	return i;
}
