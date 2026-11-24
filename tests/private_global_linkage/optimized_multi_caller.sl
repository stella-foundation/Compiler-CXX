import "private_a.sl";
import "private_b.sl";

#[abi("C"), link_name]
pub fn optimized_multi_caller() -> i32 {
	return private_a_value() + private_b_value();
}
