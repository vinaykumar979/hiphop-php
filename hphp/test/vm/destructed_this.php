<?php

function id($x) { return $x; }

class Foo {
  public function hi() {
    $bad = new Tracer();
  }
}

class Tracer {
  public function __destruct() {
    var_dump(debug_backtrace());
  }
}

function main() {
  id(new Foo())->hi();
}
main();
