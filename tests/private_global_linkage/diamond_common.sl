const DIAMOND_PRIVATE: i32 = 17;

#[abi("C"), link_name]
pub fn diamond_common_value() -> i32 {
	return DIAMOND_PRIVATE;
}
