# SolarNX rendezvous server. Build from this folder (the repository's Server/, or a copy of it):
#   docker build -t solarnx-rendezvous .
#   docker run -d --name solarnx-rendezvous --restart unless-stopped -p 19020:19020/udp \
#     -e SOLARNX_RV_ACCESS_KEY=your-key solarnx-rendezvous
FROM mcr.microsoft.com/dotnet/sdk:10.0 AS build
WORKDIR /src
COPY . .
RUN dotnet publish SolarNX.Rendezvous.csproj -c Release -o /app

FROM mcr.microsoft.com/dotnet/runtime:10.0
WORKDIR /app
COPY --from=build /app ./
EXPOSE 19020/udp
USER app
ENTRYPOINT ["dotnet", "solarnx-rendezvous.dll"]
