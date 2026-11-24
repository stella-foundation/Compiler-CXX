import "optimized_owner.sl";

#[abi("C"), link_name]
pub fn optimized_caller() -> i32 {
	return optimized_owner();
}
