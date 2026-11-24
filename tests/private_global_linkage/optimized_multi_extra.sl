import "private_a.sl";
import "unrelated.sl";
import "private_b.sl";

#[abi("C"), link_name]
pub fn optimized_multi_extra() -> i32 {
	return private_a_value() + private_b_value();
}
