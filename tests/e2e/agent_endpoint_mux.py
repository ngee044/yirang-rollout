import http.client
import http.server
import sys

S3_TARGET = ("127.0.0.1", 8333)
SQS_TARGET = ("127.0.0.1", 9324)
HOP_BY_HOP = ("transfer-encoding", "content-length", "connection", "keep-alive", "te", "trailer", "upgrade", "proxy-connection")


class RoutingHandler(http.server.BaseHTTPRequestHandler):
	protocol_version = "HTTP/1.1"

	def forward(self):
		host, port = SQS_TARGET if self.headers.get("x-amz-target") else S3_TARGET

		if self.headers.get("Transfer-Encoding"):
			self.refuse(501, "chunked request bodies are not supported")
			return

		try:
			length = int(self.headers.get("Content-Length") or 0)
		except ValueError:
			self.refuse(400, "malformed Content-Length")
			return

		body = self.rfile.read(length) if length > 0 else b""

		headers = {key: value for key, value in self.headers.items() if key.lower() not in HOP_BY_HOP and key.lower() != "expect"}
		headers["Content-Length"] = str(len(body))

		upstream = http.client.HTTPConnection(host, port, timeout=60)
		try:
			upstream.request(self.command, self.path, body=body, headers=headers)
			response = upstream.getresponse()
			payload = response.read()
		except Exception as error:
			self.refuse(502, "{}:{} — {}".format(host, port, error))
			return
		finally:
			upstream.close()

		declared = response.getheader("Content-Length") if self.command == "HEAD" else str(len(payload))

		self.send_response(response.status)
		for key, value in response.getheaders():
			if key.lower() not in HOP_BY_HOP:
				self.send_header(key, value)
		self.send_header("Content-Length", declared if declared is not None else str(len(payload)))
		self.end_headers()

		if self.command != "HEAD":
			self.wfile.write(payload)

	def refuse(self, status, reason):
		self.log_error("%s %s -> %s %s", self.command, self.path, status, reason)

		message = reason.encode()
		self.send_response(status)
		self.send_header("Content-Type", "text/plain")
		self.send_header("Content-Length", str(len(message)))
		self.end_headers()
		self.wfile.write(message)

	do_GET = forward
	do_PUT = forward
	do_POST = forward
	do_HEAD = forward
	do_DELETE = forward

	def log_message(self, *args):
		pass

	def log_error(self, message, *args):
		sys.stderr.write("[agent_endpoint_mux] " + (message % args) + "\n")
		sys.stderr.flush()


class ThreadingServer(http.server.ThreadingHTTPServer):
	daemon_threads = True
	allow_reuse_address = True


if __name__ == "__main__":
	port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
	ThreadingServer(("127.0.0.1", port), RoutingHandler).serve_forever()
