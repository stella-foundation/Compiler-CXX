mod generic.imported;
import "./generic_dependency.sl";

pub const Offset: i32 = 7;

pub trait Value {
	fn value(self: *Self) -> i32;
}

pub struct Number { pub Amount: i32, }
impl Value for Number {
	pub fn value(self: *Self) -> i32 { return self.Amount; }
}

fn helper(value: i32) -> i32 { return value + 3; }

pub fn evaluate<T: Value>(value: *T) -> i32 {
	return helper(value.value()) + Offset;
}

pub struct Envelope<T: Value> {
	pub Item: *T,
	pub State: dependency.Status,
}
