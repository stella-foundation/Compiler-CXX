const FRACUNIT: i32 = 29;

#[abi("C"), link_name]
pub fn private_b_value() -> i32 {
	return FRACUNIT;
}
