import "diamond_common.sl";

#[abi("C"), link_name]
pub fn diamond_right_value() -> i32 {
	return diamond_common_value();
}
