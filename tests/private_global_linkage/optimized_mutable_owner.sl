let mut PRIVATE_STATE: i32 = 10;

#[abi("C"), link_name]
pub fn optimized_mutable_owner() -> i32 {
	PRIVATE_STATE += 1;
	return PRIVATE_STATE;
}
