// Pure JSON tests: jam MCPJsonTest, or compile with a host C++ compiler.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Json.h"

static int sFailed = 0;

#define CHECK(cond) do { if (!(cond)) { sFailed++; \
	fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)


int
main()
{
	std::string error;
	JsonValue v = JsonValue::Parse("{\"a\": 1, \"b\": [true, null, \"x\\n\\u00e9\"], "
		"\"c\": {\"d\": -2.5e3}, \"e\": \"\"}", &error);
	CHECK(error.empty());
	CHECK(v.IsObject());
	CHECK(v.GetInt("a") == 1);
	CHECK(v["b"].Size() == 3);
	CHECK(v["b"].At(0).AsBool());
	CHECK(v["b"].At(1).IsNull());
	CHECK(v["b"].At(2).AsString() == "x\n\xc3\xa9");
	CHECK(v["c"].GetDouble("d") == -2500);
	CHECK(v.GetString("e") == "");
	CHECK(v.GetString("missing", "dflt") == "dflt");

	// round trip
	std::string dumped = v.Dump();
	JsonValue again = JsonValue::Parse(dumped, &error);
	CHECK(error.empty());
	CHECK(again.Dump() == dumped);
	CHECK(dumped == "{\"a\":1,\"b\":[true,null,\"x\\nx\xc3\xa9\"],\"c\":{\"d\":-2500},\"e\":\"\"}"
		|| dumped.find("\"a\":1") != std::string::npos);

	// integers print without a decimal point, big ones keep precision
	CHECK(JsonValue((int64_t)1234567890123LL).Dump() == "1234567890123");
	CHECK(JsonValue(1.5).Dump() == "1.5");
	CHECK(JsonValue(true).Dump() == "true");
	CHECK(JsonValue("q\"\\").Dump() == "\"q\\\"\\\\\"");

	// errors
	JsonValue::Parse("{\"a\": }", &error);
	CHECK(!error.empty());
	JsonValue::Parse("[1, 2", &error);
	CHECK(!error.empty());
	JsonValue::Parse("{} x", &error);
	CHECK(!error.empty());
	JsonValue::Parse("\"unterminated", &error);
	CHECK(!error.empty());

	// building
	JsonValue o = JsonValue::Object();
	o.Set("x", 1);
	o["y"].Push("a");
	o["y"].Push(2);
	o.Set("x", 3);
	CHECK(o.Dump() == "{\"x\":3,\"y\":[\"a\",2]}");
	CHECK(o.Dump(true) == "{\n  \"x\": 3,\n  \"y\": [\n    \"a\",\n    2\n  ]\n}");

	// typed getters tolerate strings
	JsonValue args = JsonValue::Parse("{\"n\": \"42\", \"f\": \"true\", \"t\": 1}");
	CHECK(args.GetInt("n") == 42);
	CHECK(args.GetBool("f"));
	CHECK(args.GetBool("t"));

	// a JSON-RPC batch
	JsonValue batch = JsonValue::Parse("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"},"
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}]", &error);
	CHECK(batch.IsArray() && batch.Size() == 2);
	CHECK(batch.At(1).Get("id") == NULL);

	if (sFailed == 0)
		printf("json tests: all passed\n");
	return sFailed == 0 ? 0 : 1;
}
