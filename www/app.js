fetch("/api/time")
  .then((response) => response.json())
  .then((data) => {
    document.getElementById("now").textContent = data.now;
  })
  .catch((error) => {
    document.getElementById("now").textContent = "error: " + error;
  });
