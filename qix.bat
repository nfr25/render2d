@echo off
rem qix FILE.shp [FILE2.shp ...]
rem Creates the spatial index (.qix) of each shapefile, next to it.
rem Same as: ogrinfo FILE.shp -sql "CREATE SPATIAL INDEX ON FILE"
rem (the layer name is the file name without .shp). See viewer2d notions, chapter 10.
if "%~1"=="" (
  echo usage: qix FILE.shp [FILE2.shp ...]
  exit /b 2
)
for %%f in (%*) do (
  echo %%~nf
  ogrinfo -q "%%~ff" -sql "CREATE SPATIAL INDEX ON %%~nf"
  if exist "%%~dpnf.qix" (echo   ok: %%~dpnf.qix) else (echo   no .qix created for %%~ff)
)
