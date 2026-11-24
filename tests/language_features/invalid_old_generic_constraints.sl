fn old_unsigned_constraint<T: ui>(value: T) -> T {
	return value;
}

fn old_signed_constraint<T: si>(value: T) -> T {
	return value;
}

fn reject_old_unsigned_constraint(value: u8) -> u8 {
	return old_unsigned_constraint(value);
}

fn reject_old_signed_constraint(value: i8) -> i8 {
	return old_signed_constraint(value);
}
