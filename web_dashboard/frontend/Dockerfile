# Multi-stage: build the React app with Node, then ship only the static
# output in a tiny nginx image - Node itself never reaches the Pi.
FROM node:20-alpine AS build
WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY . .
# Baked in at BUILD time (Vite inlines import.meta.env.* into the bundle) -
# "/api" means "same origin, let nginx proxy it" (see nginx.conf), instead
# of the http://localhost:8000 default used for local dev.
ARG VITE_API_URL=/api
ENV VITE_API_URL=$VITE_API_URL
RUN npm run build

# nginx-unprivileged: same nginx, but already configured to run as a
# non-root user and listen on port 8080 instead of 80 (binding <1024 needs
# root) - docker-compose.yml maps host 80 -> container 8080, so nothing
# changes from the browser's side.
FROM nginxinc/nginx-unprivileged:alpine
COPY --from=build /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf
