// Mock stream conforming to Reader and Writer traits
pub struct MemBuffer {
    data: [16]u8,
    pos: usize,
    len: usize,
}

pub trait Reader {
    fn read(self: *Self, buffer: *mut u8, capacity: usize, count_out: *mut usize) -> i32;
}

pub trait Writer {
    fn write(self: *Self, buffer: *u8, length: usize, count_out: *mut usize) -> i32;
}

pub trait Closer {
    fn close(self: *mut Self) -> i32;
}

impl Reader for MemBuffer {
    fn read(self: *Self, buffer: *mut u8, capacity: usize, count_out: *mut usize) -> i32 {
        let mut available: usize = self.len - self.pos;
        let mut to_read: usize = capacity < available ? capacity : available;
        for let mut i: usize = 0; i < to_read; i += 1 {
            buffer[i] = self.data[self.pos + i];
        }
        let self_mut: *mut MemBuffer = self cast *mut MemBuffer;
        self_mut.pos += to_read;
        count_out[0] = to_read;
        return 0;
    }
}

impl Writer for MemBuffer {
    fn write(self: *Self, buffer: *u8, length: usize, count_out: *mut usize) -> i32 {
        let mut space: usize = 16 - self.len;
        let mut to_write: usize = length < space ? length : space;
        let self_mut: *mut MemBuffer = self cast *mut MemBuffer;
        for let mut i: usize = 0; i < to_write; i += 1 {
            self_mut.data[self_mut.len + i] = buffer[i];
        }
        self_mut.len += to_write;
        count_out[0] = to_write;
        return 0;
    }
}

pub fn copy<R: Reader, W: Writer>(reader: *R, writer: *W, total_out: *mut usize) -> i32 {
    let mut buf: [4]u8;
    let mut total: usize = 0;
    for true {
        let mut n_read: usize = 0;
        let r_res: i32 = reader.read(&buf[0], 4, &n_read);
        if r_res != 0 || n_read == 0 {
            break;
        }
        let mut n_written: usize = 0;
        let w_res: i32 = writer.write(&buf[0], n_read, &n_written);
        if w_res != 0 {
            break;
        }
        total += n_written;
    }
    total_out[0] = total;
    return 0;
}

// Ord trait and zero-cost sort
pub struct Score {
    val: i32,
}

pub trait Ord {
    fn cmp(self: *Self, other: *Self) -> i32;
}

impl Ord for Score {
    fn cmp(self: *Self, other: *Self) -> i32 {
        return self.val - other.val;
    }
}

pub fn sort2<T: Ord>(a: *mut T, b: *mut T) -> void {
    if a.cmp(b) > 0 {
        let tmp: T = a[0];
        a[0] = b[0];
        b[0] = tmp;
    }
}

// Disposable trait and handle
pub trait Disposable {
    fn destroy(self: *Self) -> void;
}

pub struct Handle {
    id: i32,
    destroyed: bool,
}

impl Disposable for Handle {
    fn destroy(self: *Self) -> void {
        let h_mut: *mut Handle = self cast *mut Handle;
        h_mut.destroyed = true;
    }
}

pub fn dispose<D: Disposable>(res: *D) -> void {
    res.destroy();
}

// Tests
#[abi("C"), link_name("test_stream_copy")]
pub fn test_stream_copy() -> i32 {
    let mut src: MemBuffer;
    src.pos = 0;
    src.len = 5;
    src.data[0] = 10;
    src.data[1] = 20;
    src.data[2] = 30;
    src.data[3] = 40;
    src.data[4] = 50;

    let mut dst: MemBuffer;
    dst.pos = 0;
    dst.len = 0;
    let mut total: usize = 0;
    copy(&src, &dst, &total);

    if total != 5 || dst.len != 5 {
        return 1;
    }
    if dst.data[0] != 10 || dst.data[4] != 50 {
        return 2;
    }
    return 42;
}

#[abi("C"), link_name("test_ord_sort")]
pub fn test_ord_sort() -> i32 {
    let mut s1: Score = { .val = 99 };
    let mut s2: Score = { .val = 42 };
    sort2(&s1, &s2);
    if s1.val == 42 && s2.val == 99 {
        return 100;
    }
    return 0;
}

#[abi("C"), link_name("test_disposable")]
pub fn test_disposable() -> i32 {
    let mut h: Handle = { .id = 7, .destroyed = false };
    dispose(&h);
    return h.destroyed ? 200 : 0;
}
