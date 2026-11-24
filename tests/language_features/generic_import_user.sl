import "./generic_import.sl";

#[abi("C"), link_name("generic_import_test")]
pub fn generic_import_test() -> i32 {
	let number: generic.imported.Number = { .Amount = 32 };
	let envelope: generic.imported.Envelope<generic.imported.Number> = { .Item = &number, .State = .Good };
	return generic.imported.evaluate(envelope.Item) + (envelope.State cast i32);
}
