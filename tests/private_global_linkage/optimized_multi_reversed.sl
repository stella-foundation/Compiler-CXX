import "private_b.sl";
import "private_a.sl";

#[abi("C"), link_name]
pub fn optimized_multi_reversed() -> i32 {
	return private_a_value() + private_b_value();
}
