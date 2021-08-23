@echo off

setlocal
setlocal EnableExtensions
setlocal EnableDelayedExpansion

set offset=2_1497.030_1514.415_1501.982_0.0_0.00_0.000_1491.991_4555.739_1542.696_0.089_-0.077_179.891_6080_3040_2323

for /f "usebackq delims=|" %%f in (`dir /b "*.insv"`) do (
  set fname=%%f
  ins_file_tool -s !fname!
  set err=!ERRORLEVEL!

  if "!err!"=="0" (
    ins_file_tool -c !fname! !fname!.new %offset%
    set err=!ERRORLEVEL!
    
    if "!err!"=="0" (
      move !fname! !fname!.old
      move !fname!.new !fname!
     
      echo Done for file : !fname!
    )
  )
)

exit /b 0