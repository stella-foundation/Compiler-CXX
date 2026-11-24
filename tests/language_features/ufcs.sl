#[repr("C")]
pub struct Point {
	x: i32,
	y: i32,
}

pub fn length_sq(p: Point) -> i32 {
	return p.x * p.x + p.y * p.y;
}

#[abi("C"), link_name("test_ufcs_direct")]
pub fn test_ufcs_direct(p: Point) -> i32 {
	return p.length_sq();
}

#[abi("C"), link_name("test_direct_call")]
pub fn test_direct_call(p: Point) -> i32 {
	return length_sq(p);
}

pub fn add(x: i32, y: i32) -> i32 {
	return x + y;
}

pub fn mul(x: i32, y: i32) -> i32 {
	return x * y;
}

pub fn sub(x: i32, y: i32) -> i32 {
	return x - y;
}

#[abi("C"), link_name("test_ufcs_chain")]
pub fn test_ufcs_chain(x: i32) -> i32 {
	return x.add(5).mul(2).sub(4);
}

#[abi("C"), link_name("test_direct_chain")]
pub fn test_direct_chain(x: i32) -> i32 {
	return sub(mul(add(x, 5), 2), 4);
}

pub struct RectBuilder {
	w: i32,
	h: i32,
}

pub fn set_w(b: *mut RectBuilder, w: i32) -> *mut RectBuilder {
	b.w = w;
	return b;
}

pub fn set_h(b: *mut RectBuilder, h: i32) -> *mut RectBuilder {
	b.h = h;
	return b;
}

pub fn area(b: *RectBuilder) -> i32 {
	return b.w * b.h;
}

#[abi("C"), link_name("test_builder_chain")]
pub fn test_builder_chain() -> i32 {
	let mut b: RectBuilder = { .w = 0, .h = 0 };
	return b.set_w(10).set_h(20).area();
}

pub fn read_point(p: *Point) -> i32 {
	return p.x + p.y;
}

#[abi("C"), link_name("test_auto_ref")]
pub fn test_auto_ref() -> i32 {
	let p: Point = { .x = 10, .y = 20 };
	return p.read_point();
}

pub fn scale_point(p: *mut Point, factor: i32) -> void {
	p.x = p.x * factor;
	p.y = p.y * factor;
}

#[abi("C"), link_name("test_auto_ref_mut")]
pub fn test_auto_ref_mut() -> i32 {
	let mut p: Point = { .x = 3, .y = 4 };
	p.scale_point(2);
	return p.x + p.y;
}

#[abi("C"), link_name("test_auto_deref")]
pub fn test_auto_deref(ptr: *Point) -> i32 {
	return ptr.length_sq();
}

pub struct Vec2 {
	x: f32,
	y: f32,
}

impl Vec2 {
	pub const DEFAULT_X: f32 = 1.5;

	pub fn new(x: f32, y: f32) -> Self {
		return { .x = x, .y = y };
	}

	pub fn length(v: *Self) -> f32 {
		return v.x + v.y;
	}
}

impl Vec2 {
	pub fn default_vec() -> Self {
		return Self.new(Self.DEFAULT_X, 2.5);
	}
}

#[abi("C"), link_name("test_struct_scoped_ufcs")]
pub fn test_struct_scoped_ufcs() -> f32 {
	let v: Vec2 = Vec2.default_vec();
	return v.length();
}

pub struct Cell<T> {
	value: T,
}

impl<T> Cell<T> {
	pub fn make(val: T) -> Self {
		return { .value = val };
	}

	pub fn get(self: *Self) -> T {
		return self.value;
	}
}

impl Cell<T> {
	pub fn map<U>(self: *Self, mapper: fn(T) -> U) -> Cell<U> {
		return Cell<U>.make(mapper(self.value));
	}
}

pub mod math_ns {
	pub mod nested {
		pub fn add_ten(x: i32) -> i32 {
			return x + 10;
		}
	}
}

pub mod math_ns {
	pub fn sub_ten(x: i32) -> i32 {
		return x - 10;
	}
}

#[abi("C"), link_name("test_mod_and_generic_impl")]
pub fn test_mod_and_generic_impl(val: i32) -> i32 {
	let c1: Cell<i32> = Cell<i32>.make(val);
	let c2: Cell<i32> = c1.map<i32>(helper_double);
	let c3: Cell<i32> = Cell.make(c2.get());
	return math_ns.sub_ten(math_ns.nested.add_ten(c3.get()));
}

pub fn identity<T>(val: T) -> T {
	return val;
}

pub fn wrap<T>(val: T) -> Cell<T> {
	return { .value = val };
}

pub fn unwrap<T>(c: Cell<T>) -> T {
	return c.value;
}

#[abi("C"), link_name("test_generic_ufcs")]
pub fn test_generic_ufcs(val: i32) -> i32 {
	return val.wrap().unwrap().identity();
}

pub fn abs(val: i32) -> i32 {
	return val < 0 ? -val : val;
}

#[abi("C"), link_name("test_primitive_ufcs")]
pub fn test_primitive_ufcs(val: i32) -> i32 {
	return val.abs();
}

pub struct CallbackObj {
	cb: fn(i32) -> i32,
}

pub fn cb(_: CallbackObj, val: i32) -> i32 {
	return val * 1000;
}

pub fn helper_double(x: i32) -> i32 {
	return x * 2;
}

#[abi("C"), link_name("test_priority1_fn_ptr")]
pub fn test_priority1_fn_ptr() -> i32 {
	let obj: CallbackObj = { .cb = helper_double };
	return obj.cb(21);
}

#[abi("C"), link_name("test_ufcs_point_call")]
pub fn test_ufcs_point_call() -> i32 {
	let p: Point = { .x = 3, .y = 4 };
	return p.length_sq();
}
