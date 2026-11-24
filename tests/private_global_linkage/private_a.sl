const FRACUNIT: i32 = 11;

#[abi("C"), link_name]
pub fn private_a_value() -> i32 {
	return FRACUNIT;
}
