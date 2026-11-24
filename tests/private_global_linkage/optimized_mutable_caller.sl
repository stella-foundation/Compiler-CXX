import "optimized_mutable_owner.sl";

#[abi("C"), link_name]
pub fn optimized_mutable_caller() -> i32 {
	return optimized_mutable_owner();
}
