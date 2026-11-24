// POST a small body to the built-in /echo handler and show the reply.
document.getElementById("echo-btn").addEventListener("click", async () => {
  const out = document.getElementById("echo-out");
  try {
    const res = await fetch("/echo?from=js", {
      method: "POST",
      headers: { "Content-Type": "text/plain" },
      body: "hello from app.js at " + new Date().toISOString(),
    });
    out.textContent = res.status + " " + res.statusText + "\n\n" + (await res.text());
  } catch (err) {
    out.textContent = "request failed: " + err;
  }
});
