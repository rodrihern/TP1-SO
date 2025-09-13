var links;

function togglePathView() {
  console.log("toggle")
  for (var i = 0, len = links.length; i < len; i++) {
    console.log(links[i])
    links[i].classList.toggle("display-none");
  }
}

document.addEventListener('DOMContentLoaded', function() {
  links = document.getElementsByClassName("file-link");
  console.log(links)
}, false);