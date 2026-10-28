const APEX = "example.com";

export default {
  fetch(request) {
    const url = new URL(request.url);
    url.protocol = "https:";
    url.hostname = APEX;
    url.port = "";
    return Response.redirect(url.toString(), 301);
  },
};
