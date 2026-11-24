const UNRELATED_PRIVATE: i32 = 1000;

#[abi("C"), link_name]
pub fn unrelated_value() -> i32 {
	return UNRELATED_PRIVATE;
}
