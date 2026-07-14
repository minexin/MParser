obj = pkgchild.Counter(5, 7);

base_code = obj.baseCode();
child_code = obj.childCode();
dynamic_code = obj.dispatchCode();
static_code = pkgchild.Counter.tag();
nested_code = pkgchild.inner.Marker.code();

other = pkgsibling.Counter(9);
other_code = other.code();
