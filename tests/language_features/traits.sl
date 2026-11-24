pub struct Point {
    x: i32,
    y: i32,
}

pub trait Printable {
    fn print(p: *Self) -> i32;
}

pub trait Doubler {
    fn double_it(p: *Self) -> i32;
}

impl Printable for Point {
    fn print(p: *Self) -> i32 {
        return p.x + p.y;
    }
}

impl Doubler for Point {
    fn double_it(p: *Point) -> i32 {
        return (p.x + p.y) * 2;
    }
}

pub trait ValueOp {
    fn compute(v: i32) -> i32;
}

impl ValueOp for i32 {
    fn compute(v: i32) -> i32 {
        return v * 10;
    }
}

pub fn display<T: Printable>(item: *T) -> i32 {
    return item.print();
}

pub fn do_both<T: Printable + Doubler>(item: *T) -> i32 {
    return item.print() + item.double_it();
}

#[abi("C"), link_name("test_trait_basic")]
pub fn test_trait_basic() -> i32 {
    let pt: Point = { .x = 10, .y = 20 };
    return pt.print();
}

#[abi("C"), link_name("test_trait_generic")]
pub fn test_trait_generic() -> i32 {
    let pt: Point = { .x = 5, .y = 15 };
    return display(&pt);
}

#[abi("C"), link_name("test_trait_multiple_bounds")]
pub fn test_trait_multiple_bounds() -> i32 {
    let pt: Point = { .x = 3, .y = 7 };
    return do_both(&pt);
}

#[abi("C"), link_name("test_trait_primitive")]
pub fn test_trait_primitive() -> i32 {
    let x: i32 = 7;
    return x.compute();
}

pub struct Box<T> {
    value: T,
}

impl<T: Printable> Printable for Box<T> {
    fn print(self: *Self) -> i32 {
        return self.value.print() + 100;
    }
}

#[abi("C"), link_name("test_trait_generic_impl")]
pub fn test_trait_generic_impl() -> i32 {
    let b: Box<Point> = { .value = { .x = 10, .y = 20 } };
    return display(&b) + b.print();
}
