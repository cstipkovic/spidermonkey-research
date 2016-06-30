/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

// Test cases borrowed and adapted from:
// https://github.com/joyent/node/blob/6101eb184db77d0b11eb96e48744e57ecce4b73d/test/simple/test-assert.js
// MIT license: http://opensource.org/licenses/MIT

function run_test() {
  let ns = {};
  Components.utils.import("resource://testing-common/Assert.jsm", ns);
  let assert = new ns.Assert();

  function makeBlock(f, ...args) {
    return function() {
      return f.apply(assert, args);
    };
  }

  function protoCtrChain(o) {
    let result = [];
    while (o = o.__proto__) {
      result.push(o.constructor);
    }
    return result.join();
  }

  function indirectInstanceOf(obj, cls) {
    if (obj instanceof cls) {
      return true;
    }
    let clsChain = protoCtrChain(cls.prototype);
    let objChain = protoCtrChain(obj);
    return objChain.slice(-clsChain.length) === clsChain;
  };

  assert.ok(indirectInstanceOf(ns.Assert.AssertionError.prototype, Error),
            "Assert.AssertionError instanceof Error");

  assert.throws(makeBlock(assert.ok, false),
                ns.Assert.AssertionError, "ok(false)");

  assert.ok(true, "ok(true)");

  assert.ok("test", "ok('test')");

  assert.throws(makeBlock(assert.equal, true, false), ns.Assert.AssertionError, "equal");

  assert.equal(null, null, "equal");

  assert.equal(undefined, undefined, "equal");

  assert.equal(null, undefined, "equal");

  assert.equal(true, true, "equal");

  assert.notEqual(true, false, "notEqual");

  assert.throws(makeBlock(assert.notEqual, true, true),
                ns.Assert.AssertionError, "notEqual");

  assert.throws(makeBlock(assert.strictEqual, 2, "2"),
                ns.Assert.AssertionError, "strictEqual");

  assert.throws(makeBlock(assert.strictEqual, null, undefined),
                ns.Assert.AssertionError, "strictEqual");

  assert.notStrictEqual(2, "2", "notStrictEqual");

  // deepEquals joy!
  // 7.2
  assert.deepEqual(new Date(2000, 3, 14), new Date(2000, 3, 14), "deepEqual date");
  assert.deepEqual(new Date(NaN), new Date(NaN), "deepEqual invalid dates");

  assert.throws(makeBlock(assert.deepEqual, new Date(), new Date(2000, 3, 14)),
                ns.Assert.AssertionError,
                "deepEqual date");

  // 7.3
  assert.deepEqual(/a/, /a/);
  assert.deepEqual(/a/g, /a/g);
  assert.deepEqual(/a/i, /a/i);
  assert.deepEqual(/a/m, /a/m);
  assert.deepEqual(/a/igm, /a/igm);
  assert.throws(makeBlock(assert.deepEqual, /ab/, /a/));
  assert.throws(makeBlock(assert.deepEqual, /a/g, /a/));
  assert.throws(makeBlock(assert.deepEqual, /a/i, /a/));
  assert.throws(makeBlock(assert.deepEqual, /a/m, /a/));
  assert.throws(makeBlock(assert.deepEqual, /a/igm, /a/im));

  let re1 = /a/;
  re1.lastIndex = 3;
  assert.throws(makeBlock(assert.deepEqual, re1, /a/));

  // 7.4
  assert.deepEqual(4, "4", "deepEqual == check");
  assert.deepEqual(true, 1, "deepEqual == check");
  assert.throws(makeBlock(assert.deepEqual, 4, "5"),
                ns.Assert.AssertionError,
                "deepEqual == check");

  // 7.5
  // having the same number of owned properties && the same set of keys
  assert.deepEqual({a: 4}, {a: 4});
  assert.deepEqual({a: 4, b: "2"}, {a: 4, b: "2"});
  assert.deepEqual([4], ["4"]);
  assert.throws(makeBlock(assert.deepEqual, {a: 4}, {a: 4, b: true}),
                ns.Assert.AssertionError);
  assert.deepEqual(["a"], {0: "a"});

  let a1 = [1, 2, 3];
  let a2 = [1, 2, 3];
  a1.a = "test";
  a1.b = true;
  a2.b = true;
  a2.a = "test";
  assert.throws(makeBlock(assert.deepEqual, Object.keys(a1), Object.keys(a2)),
                ns.Assert.AssertionError);
  assert.deepEqual(a1, a2);

  let nbRoot = {
    toString: function() { return this.first + " " + this.last; }
  };

  function nameBuilder(first, last) {
    this.first = first;
    this.last = last;
    return this;
  }
  nameBuilder.prototype = nbRoot;

  function nameBuilder2(first, last) {
    this.first = first;
    this.last = last;
    return this;
  }
  nameBuilder2.prototype = nbRoot;

  let nb1 = new nameBuilder("Ryan", "Dahl");
  let nb2 = new nameBuilder2("Ryan", "Dahl");

  assert.deepEqual(nb1, nb2);

  nameBuilder2.prototype = Object;
  nb2 = new nameBuilder2("Ryan", "Dahl");
  assert.throws(makeBlock(assert.deepEqual, nb1, nb2), ns.Assert.AssertionError);

  // String literal + object
  assert.throws(makeBlock(assert.deepEqual, "a", {}), ns.Assert.AssertionError);

  // Testing the throwing
  function thrower(errorConstructor) {
    throw new errorConstructor("test");
  }
  let aethrow = makeBlock(thrower, ns.Assert.AssertionError);
  aethrow = makeBlock(thrower, ns.Assert.AssertionError);

  // the basic calls work
  assert.throws(makeBlock(thrower, ns.Assert.AssertionError),
                ns.Assert.AssertionError, "message");
  assert.throws(makeBlock(thrower, ns.Assert.AssertionError), ns.Assert.AssertionError);
  assert.throws(makeBlock(thrower, ns.Assert.AssertionError));

  // if not passing an error, catch all.
  assert.throws(makeBlock(thrower, TypeError));

  // when passing a type, only catch errors of the appropriate type
  let threw = false;
  try {
    assert.throws(makeBlock(thrower, TypeError), ns.Assert.AssertionError);
  } catch (e) {
    threw = true;
    assert.ok(e instanceof TypeError, "type");
  }
  assert.equal(true, threw,
               "Assert.throws with an explicit error is eating extra errors",
               ns.Assert.AssertionError);
  threw = false;

  function ifError(err) {
    if (err) {
      throw err;
    }
  }
  assert.throws(function() {
    ifError(new Error("test error"));
  });

  // make sure that validating using constructor really works
  threw = false;
  try {
    assert.throws(
      function() {
        throw ({});
      },
      Array
    );
  } catch (e) {
    threw = true;
  }
  assert.ok(threw, "wrong constructor validation");

  // use a RegExp to validate error message
  assert.throws(makeBlock(thrower, TypeError), /test/);

  // use a fn to validate error object
  assert.throws(makeBlock(thrower, TypeError), function(err) {
    if ((err instanceof TypeError) && /test/.test(err)) {
      return true;
    }
  });

  function testAssertionMessage(actual, expected) {
    try {
      assert.equal(actual, "");
    } catch (e) {
      assert.equal(e.toString(),
          ["AssertionError:", expected, "==", '""'].join(" "));
    }
  }
  testAssertionMessage(undefined, '"undefined"');
  testAssertionMessage(null, "null");
  testAssertionMessage(true, "true");
  testAssertionMessage(false, "false");
  testAssertionMessage(0, "0");
  testAssertionMessage(100, "100");
  testAssertionMessage(NaN, '"NaN"');
  testAssertionMessage(Infinity, '"Infinity"');
  testAssertionMessage(-Infinity, '"-Infinity"');
  testAssertionMessage("", '""');
  testAssertionMessage("foo", '"foo"');
  testAssertionMessage([], "[]");
  testAssertionMessage([1, 2, 3], "[1,2,3]");
  testAssertionMessage(/a/, '"/a/"');
  testAssertionMessage(/abc/gim, '"/abc/gim"');
  testAssertionMessage(function f() {}, '"function f() {}"');
  testAssertionMessage({}, "{}");
  testAssertionMessage({a: undefined, b: null}, '{"a":"undefined","b":null}');
  testAssertionMessage({a: NaN, b: Infinity, c: -Infinity},
      '{"a":"NaN","b":"Infinity","c":"-Infinity"}');

  // https://github.com/joyent/node/issues/2893
  try {
    assert.throws(function () {
      ifError(null);
    });
  } catch (e) {
    threw = true;
    assert.equal(e.message, "Missing expected exception..");
  }
  assert.ok(threw);

  // https://github.com/joyent/node/issues/5292
  try {
    assert.equal(1, 2);
  } catch (e) {
    assert.equal(e.toString().split("\n")[0], "AssertionError: 1 == 2")
  }

  try {
    assert.equal(1, 2, "oh no");
  } catch (e) {
    assert.equal(e.toString().split("\n")[0], "AssertionError: oh no - 1 == 2")
  }

  // Test XPCShell-test integration:
  ok(true, "OK, this went well");
  deepEqual(/a/g, /a/g, "deep equal should work on RegExp");
  deepEqual(/a/igm, /a/igm, "deep equal should work on RegExp");
  deepEqual({a: 4, b: "1"}, {b: "1", a: 4}, "deep equal should work on regular Object");
  deepEqual(a1, a2, "deep equal should work on Array with Object properties");

  // Test robustness of reporting:
  equal(new ns.Assert.AssertionError({
    actual: {
      toJSON: function() {
        throw "bam!";
      }
    },
    expected: "foo",
    operator: "="
  }).message, "[object Object] = \"foo\"");

  let message;
  assert.greater(3, 2);
  try {
    assert.greater(2, 2);
  } catch(e) {
    message = e.toString().split("\n")[0];
  }
  assert.equal(message, "AssertionError: 2 > 2");

  assert.greaterOrEqual(2, 2);
  try {
    assert.greaterOrEqual(1, 2);
  } catch(e) {
    message = e.toString().split("\n")[0];
  }
  assert.equal(message, "AssertionError: 1 >= 2");

  assert.less(1, 2);
  try {
    assert.less(2, 2);
  } catch(e) {
    message = e.toString().split("\n")[0];
  }
  assert.equal(message, "AssertionError: 2 < 2");

  assert.lessOrEqual(2, 2);
  try {
    assert.lessOrEqual(2, 1);
  } catch(e) {
    message = e.toString().split("\n")[0];
  }
  assert.equal(message, "AssertionError: 2 <= 1");

  run_next_test();
}

add_task(function* test_rejects() {
  let ns = {};
  Components.utils.import("resource://testing-common/Assert.jsm", ns);
  let assert = new ns.Assert();

  // A helper function to test failures.
  function* checkRejectsFails(err, expected) {
    try {
      yield assert.rejects(Promise.reject(err), expected);
      ok(false, "should have thrown");
    } catch(ex) {
      deepEqual(ex, err, "Assert.rejects threw the original unexpected error");
    }
  }

  // A "throwable" error that's not an actual Error().
  let SomeErrorLikeThing = function() {};

  // The actual tests...
  // No "expected" or "message" values supplied.
  yield assert.rejects(Promise.reject(new Error("oh no")));
  yield assert.rejects(Promise.reject("oh no"));

  // An explicit error object:
  // An instance to check against.
  yield assert.rejects(Promise.reject(new Error("oh no")), Error, "rejected");
  // A regex to match against the message.
  yield assert.rejects(Promise.reject(new Error("oh no")), /oh no/, "rejected");

  // Failure cases:
  // An instance to check against that doesn't match.
  yield checkRejectsFails(new Error("something else"), SomeErrorLikeThing);
  // A regex that doesn't match.
  yield checkRejectsFails(new Error("something else"), /oh no/);

  // Check simple string messages.
  yield assert.rejects(Promise.reject("oh no"), /oh no/, "rejected");
  // Wrong message.
  yield checkRejectsFails("something else", /oh no/);
});
