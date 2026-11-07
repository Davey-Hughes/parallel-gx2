#!/bin/sh
# curl_compat against local servers: GET, POST from a buffer, from a
# read callback and as a multipart form, a 404, a timeout, the multi interface with and without
# paused transfers, several threads at once, the transfer thread stopped
# and started again, and TLS with the client certificate and the
# RSA/AES-256-CBC suite nlibcurl asks for, against a server that requires
# both - and refused with a trust anchor that signed nothing. Needs
# python3 and openssl. RUNNER prefixes the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
D=$(mktemp -d)
trap 'kill $HTTP $TLS 2>/dev/null || true; rm -rf $D' EXIT
make -s
RUN=${RUNNER:-}
# a case that hangs fails rather than holding the job
command -v timeout >/dev/null 2>&1 && RUN="timeout 120 $RUN"
EXE=${EXE:-}
head -c 1048579 /dev/urandom > $D/body.bin
python3 -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" $D/body.bin > $D/body.sha256
openssl req -x509 -newkey rsa:2048 -nodes -keyout $D/ca.key -out $D/ca.pem -days 2 -subj "/CN=test CA" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout $D/srv.key -out $D/srv.csr -subj "/CN=localhost" 2>/dev/null
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > $D/srv.ext
openssl x509 -req -in $D/srv.csr -CA $D/ca.pem -CAkey $D/ca.key -CAcreateserial -out $D/srv.pem -days 2 -extfile $D/srv.ext 2>/dev/null
openssl x509 -in $D/ca.pem -outform DER -out $D/ca.der
openssl req -x509 -newkey rsa:2048 -nodes -keyout $D/cca.key -out $D/cca.pem -days 2 -subj "/CN=client CA" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout $D/cli.key -out $D/cli.csr -subj "/CN=client" 2>/dev/null
openssl x509 -req -in $D/cli.csr -CA $D/cca.pem -CAkey $D/cca.key -CAcreateserial -out $D/cli.pem -days 2 2>/dev/null
openssl x509 -in $D/cli.pem -outform DER -out $D/cli.der
openssl rsa -in $D/cli.key -traditional -outform DER -out $D/cli.key.der 2>/dev/null
HP=${HTTP_PORT:-45180}
TP=${TLS_PORT:-45443}
python3 server.py $HP $D & HTTP=$!
(cd $D && exec openssl s_server -accept $TP -cert srv.pem -key srv.key -tls1_2 -cipher AES256-SHA \
   -Verify 1 -CAfile cca.pem -WWW) >/dev/null 2>&1 & TLS=$!
sleep 1
fail=0
for t in get post post-read form 404 timeout multi pause threads restart tls tls-refused; do
   if $RUN ./curl_compat_test$EXE $t $HP $TP $D > $D/out 2>&1; then echo "ok:   $t"
   else echo "FAIL: $t: $(cat $D/out)"; fail=1; fi
done
[ $fail -eq 0 ] && echo "[pass] curl_compat"
exit $fail
