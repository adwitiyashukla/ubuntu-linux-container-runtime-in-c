#!/bin/bash
set -eu

dest=$1
bb=$(command -v busybox || true)
if [ -z "$bb" ]; then
	echo "busybox is required to build the test rootfs (apt install busybox-static)" >&2
	exit 1
fi

mkdir -p "$dest"/bin "$dest"/sbin "$dest"/etc "$dest"/proc "$dest"/sys "$dest"/dev "$dest"/tmp "$dest"/root "$dest"/www "$dest"/usr/bin "$dest"/usr/sbin
cp "$bb" "$dest/bin/busybox"
for applet in $("$dest/bin/busybox" --list); do
	[ "$applet" = busybox ] && continue
	ln -sf busybox "$dest/bin/$applet"
done

if ldd "$bb" >/dev/null 2>&1; then
	for lib in $(ldd "$bb" | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i }'); do
		mkdir -p "$dest$(dirname "$lib")"
		cp "$lib" "$dest$lib"
	done
fi

printf 'root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/:/bin/false\n' > "$dest/etc/passwd"
printf 'root:x:0:\nnogroup:x:65534:\n' > "$dest/etc/group"
echo "hello from lcr" > "$dest/www/index.html"
chmod 1777 "$dest/tmp"
