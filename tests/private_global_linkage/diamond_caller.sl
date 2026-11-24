import "diamond_right.sl";
import "diamond_left.sl";

#[abi("C"), link_name]
pub fn diamond_caller() -> i32 {
	return diamond_left_value() + diamond_right_value();
}
