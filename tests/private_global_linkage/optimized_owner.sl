const FRACUNIT: i32 = 37;

#[abi("C"), link_name]
pub fn optimized_owner() -> i32 {
	return FRACUNIT + 5;
}
