FROM node:20-alpine

WORKDIR /app

# Install dependencies dulu (memanfaatkan layer cache Docker)
COPY package.json ./
RUN npm install --omit=dev

# Salin source code & file statis dashboard
COPY server.js ./
COPY public ./public

EXPOSE 4000

ENV PORT=4000

CMD ["node", "server.js"]
