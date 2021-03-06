/** @file

  Copyright (c) 2016 - 2020, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
    - Sbbr or SBBR   - Server Base Boot Requirements

  @par Reference(s):
    - Arm Server Base Boot Requirements 1.2, September 2019
**/

#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/ShellLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include "AcpiParser.h"
#include "AcpiTableParser.h"
#include "AcpiView.h"
#include "UefiShellAcpiViewCommandLib.h"

#if defined(MDE_CPU_ARM) || defined (MDE_CPU_AARCH64)
#include "Arm/SbbrValidator.h"
#endif

EFI_HII_HANDLE gShellAcpiViewHiiHandle = NULL;

// Report variables
STATIC UINT32             mSelectedAcpiTable;
STATIC CONST CHAR16*      mSelectedAcpiTableName;
STATIC BOOLEAN            mSelectedAcpiTableFound;
STATIC EREPORT_OPTION     mReportType;
STATIC UINT32             mTableCount;
STATIC UINT32             mBinTableCount;
STATIC BOOLEAN            mConsistencyCheck;
STATIC BOOLEAN            mColourHighlighting;
STATIC BOOLEAN            mMandatoryTableValidate;
STATIC UINTN              mMandatoryTableSpec;

/**
  An array of acpiview command line parameters.
**/
STATIC CONST SHELL_PARAM_ITEM ParamList[] = {
  {L"-q", TypeFlag},
  {L"-d", TypeFlag},
  {L"-h", TypeFlag},
  {L"-l", TypeFlag},
  {L"-s", TypeValue},
  {L"-r", TypeValue},
  {NULL, TypeMax}
};

/**
  This function returns the colour highlighting status.

  @retval TRUE if colour highlighting is enabled.
**/
BOOLEAN
GetColourHighlighting (
  VOID
  )
{
  return mColourHighlighting;
}

/**
  This function sets the colour highlighting status.

  @param  Highlight       The Highlight status.

**/
VOID
SetColourHighlighting (
  BOOLEAN Highlight
  )
{
  mColourHighlighting = Highlight;
}

/**
  This function returns the consistency checking status.

  @retval TRUE if consistency checking is enabled.
**/
BOOLEAN
GetConsistencyChecking (
  VOID
  )
{
  return mConsistencyCheck;
}

/**
  This function sets the consistency checking status.

  @param  ConsistencyChecking   The consistency checking status.

**/
VOID
SetConsistencyChecking (
  BOOLEAN ConsistencyChecking
  )
{
  mConsistencyCheck = ConsistencyChecking;
}

/**
  This function returns the ACPI table requirements validation flag.

  @retval TRUE if check for mandatory table presence should be performed.
**/
BOOLEAN
GetMandatoryTableValidate (
  VOID
  )
{
  return mMandatoryTableValidate;
}

/**
  This function sets the ACPI table requirements validation flag.

  @param  Validate    Enable/Disable ACPI table requirements validation.
**/
VOID
SetMandatoryTableValidate (
  BOOLEAN Validate
  )
{
  mMandatoryTableValidate = Validate;
}

/**
  This function returns the identifier of specification to validate ACPI table
  requirements against.

  @return   ID of specification listing mandatory tables.
**/
UINTN
GetMandatoryTableSpec (
  VOID
  )
{
  return mMandatoryTableSpec;
}

/**
  This function sets the identifier of specification to validate ACPI table
  requirements against.

  @param  Spec      ID of specification listing mandatory tables.
**/
VOID
SetMandatoryTableSpec (
  UINTN Spec
  )
{
  mMandatoryTableSpec = Spec;
}

/**
  This function returns the report options.

  @retval Returns the report option.
**/
STATIC
EREPORT_OPTION
GetReportOption (
  VOID
  )
{
  return mReportType;
}

/**
  This function returns the selected ACPI table.

  @retval Returns signature of the selected ACPI table.
**/
STATIC
UINT32
GetSelectedAcpiTable (
  VOID
  )
{
  return mSelectedAcpiTable;
}

/**
  This function dumps the ACPI table to a file.

  @param [in] Ptr       Pointer to the ACPI table data.
  @param [in] Length    The length of the ACPI table.

  @retval TRUE          Success.
  @retval FALSE         Failure.
**/
STATIC
BOOLEAN
DumpAcpiTableToFile (
  IN CONST UINT8*  Ptr,
  IN CONST UINTN   Length
  )
{
  EFI_STATUS         Status;
  CHAR16             FileNameBuffer[MAX_FILE_NAME_LEN];
  SHELL_FILE_HANDLE  DumpFileHandle;
  UINTN              TransferBytes;

  DumpFileHandle = NULL;
  TransferBytes = Length;

  UnicodeSPrint (
    FileNameBuffer,
    sizeof (FileNameBuffer),
    L".\\%s%04d.bin",
    mSelectedAcpiTableName,
    mBinTableCount++
    );

  Status = ShellOpenFileByName (
             FileNameBuffer,
             &DumpFileHandle,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             0
             );
  if (EFI_ERROR (Status)) {
    ShellPrintHiiEx (
      -1,
      -1,
      NULL,
      STRING_TOKEN (STR_GEN_READONLY_MEDIA),
      gShellAcpiViewHiiHandle,
      L"acpiview"
      );
    return FALSE;
  }

  Print (L"Dumping ACPI table to : %s ... ", FileNameBuffer);

  Status = ShellWriteFile (
             DumpFileHandle,
             &TransferBytes,
             (VOID*)Ptr
             );
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: Failed to dump table to binary file.\n");
    TransferBytes = 0;
  } else {
    Print (L"DONE.\n");
  }

  ShellCloseFile (&DumpFileHandle);
  return (Length == TransferBytes);
}

/**
  This function processes the table reporting options for the ACPI table.

  @param [in] Signature The ACPI table Signature.
  @param [in] TablePtr  Pointer to the ACPI table data.
  @param [in] Length    The length fo the ACPI table.

  @retval Returns TRUE if the ACPI table should be traced.
**/
BOOLEAN
ProcessTableReportOptions (
  IN CONST UINT32  Signature,
  IN CONST UINT8*  TablePtr,
  IN CONST UINT32  Length
  )
{
  UINTN   OriginalAttribute;
  UINT8*  SignaturePtr;
  BOOLEAN Log;
  BOOLEAN HighLight;

  //
  // set local variables to suppress incorrect compiler/analyzer warnings
  //
  OriginalAttribute = 0;
  SignaturePtr = (UINT8*)(UINTN)&Signature;
  Log = FALSE;
  HighLight = GetColourHighlighting ();

  switch (GetReportOption ()) {
    case ReportAll:
      Log = TRUE;
      break;
    case ReportSelected:
      if (Signature == GetSelectedAcpiTable ()) {
        Log = TRUE;
        mSelectedAcpiTableFound = TRUE;
      }
      break;
    case ReportTableList:
      if (mTableCount == 0) {
        if (HighLight) {
          OriginalAttribute = gST->ConOut->Mode->Attribute;
          gST->ConOut->SetAttribute (
                         gST->ConOut,
                         EFI_TEXT_ATTR(EFI_CYAN,
                           ((OriginalAttribute&(BIT4|BIT5|BIT6))>>4))
                         );
        }
        Print (L"\nInstalled Table(s):\n");
        if (HighLight) {
          gST->ConOut->SetAttribute (gST->ConOut, OriginalAttribute);
        }
      }
      Print (
        L"\t%4d. %c%c%c%c\n",
        ++mTableCount,
        SignaturePtr[0],
        SignaturePtr[1],
        SignaturePtr[2],
        SignaturePtr[3]
        );
      break;
    case ReportDumpBinFile:
      if (Signature == GetSelectedAcpiTable ()) {
        mSelectedAcpiTableFound = TRUE;
        DumpAcpiTableToFile (TablePtr, Length);
      }
      break;
    case ReportMax:
      // We should never be here.
      // This case is only present to prevent compiler warning.
      break;
  } // switch

  if (Log) {
    if (HighLight) {
      OriginalAttribute = gST->ConOut->Mode->Attribute;
      gST->ConOut->SetAttribute (
                     gST->ConOut,
                     EFI_TEXT_ATTR(EFI_LIGHTBLUE,
                       ((OriginalAttribute&(BIT4|BIT5|BIT6))>>4))
                     );
    }
    Print (
      L"\n\n --------------- %c%c%c%c Table --------------- \n\n",
      SignaturePtr[0],
      SignaturePtr[1],
      SignaturePtr[2],
      SignaturePtr[3]
      );
    if (HighLight) {
      gST->ConOut->SetAttribute (gST->ConOut, OriginalAttribute);
    }
  }

  return Log;
}

/**
  This function converts a string to ACPI table signature.

  @param [in] Str   Pointer to the string to be converted to the
                    ACPI table signature.

  @retval The ACPI table signature.
**/
STATIC
UINT32
ConvertStrToAcpiSignature (
  IN  CONST CHAR16* Str
  )
{
  UINT8 Index;
  CHAR8 Ptr[4];

  ZeroMem (Ptr, sizeof (Ptr));
  Index = 0;

  // Convert to Upper case and convert to ASCII
  while ((Index < 4) && (Str[Index] != 0)) {
    if (Str[Index] >= L'a' && Str[Index] <= L'z') {
      Ptr[Index] = (CHAR8)(Str[Index] - (L'a' - L'A'));
    } else {
      Ptr[Index] = (CHAR8)Str[Index];
    }
    Index++;
  }
  return *(UINT32*)Ptr;
}

/**
  This function iterates the configuration table entries in the
  system table, retrieves the RSDP pointer and starts parsing the ACPI tables.

  @param [in] SystemTable Pointer to the EFI system table.

  @retval Returns EFI_NOT_FOUND   if the RSDP pointer is not found.
          Returns EFI_UNSUPPORTED if the RSDP version is less than 2.
          Returns EFI_SUCCESS     if successful.
**/
STATIC
EFI_STATUS
EFIAPI
AcpiView (
  IN EFI_SYSTEM_TABLE* SystemTable
  )
{
  EFI_STATUS               Status;
  UINTN                    Index;
  EFI_CONFIGURATION_TABLE* EfiConfigurationTable;
  BOOLEAN                  FoundAcpiTable;
  UINTN                    OriginalAttribute;
  UINTN                    PrintAttribute;
  EREPORT_OPTION           ReportOption;
  UINT8*                   RsdpPtr;
  UINT32                   RsdpLength;
  UINT8                    RsdpRevision;
  PARSE_ACPI_TABLE_PROC    RsdpParserProc;
  BOOLEAN                  Trace;

  //
  // set local variables to suppress incorrect compiler/analyzer warnings
  //
  EfiConfigurationTable = NULL;
  OriginalAttribute = 0;

  // Search the table for an entry that matches the ACPI Table Guid
  FoundAcpiTable = FALSE;
  for (Index = 0; Index < SystemTable->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gEfiAcpiTableGuid,
          &(SystemTable->ConfigurationTable[Index].VendorGuid))) {
      EfiConfigurationTable = &SystemTable->ConfigurationTable[Index];
      FoundAcpiTable = TRUE;
      break;
    }
  }

  if (FoundAcpiTable) {
    RsdpPtr = (UINT8*)EfiConfigurationTable->VendorTable;

    // The RSDP revision is 1 byte starting at offset 15
    RsdpRevision = *(RsdpPtr + RSDP_REVISION_OFFSET);

    if (RsdpRevision < 2) {
      Print (
        L"ERROR: RSDP version less than 2 is not supported.\n"
        );
      return EFI_UNSUPPORTED;
    }

#if defined(MDE_CPU_ARM) || defined (MDE_CPU_AARCH64)
    if (GetMandatoryTableValidate ()) {
      ArmSbbrResetTableCounts ();
    }
#endif

    // The RSDP length is 4 bytes starting at offset 20
    RsdpLength = *(UINT32*)(RsdpPtr + RSDP_LENGTH_OFFSET);

    Trace = ProcessTableReportOptions (RSDP_TABLE_INFO, RsdpPtr, RsdpLength);

    Status = GetParser (RSDP_TABLE_INFO, &RsdpParserProc);
    if (EFI_ERROR (Status)) {
      Print (
        L"ERROR: No registered parser found for RSDP.\n"
        );
      return Status;
    }

    RsdpParserProc (
      Trace,
      RsdpPtr,
      RsdpLength,
      RsdpRevision
      );

  } else {
    IncrementErrorCount ();
    Print (
      L"ERROR: Failed to find ACPI Table Guid in System Configuration Table.\n"
      );
    return EFI_NOT_FOUND;
  }

#if defined(MDE_CPU_ARM) || defined (MDE_CPU_AARCH64)
  if (GetMandatoryTableValidate ()) {
    ArmSbbrReqsValidate ((ARM_SBBR_VERSION)GetMandatoryTableSpec ());
  }
#endif

  ReportOption = GetReportOption ();
  if (ReportTableList != ReportOption) {
    if (((ReportSelected == ReportOption)  ||
         (ReportDumpBinFile == ReportOption)) &&
        (!mSelectedAcpiTableFound)) {
      Print (L"\nRequested ACPI Table not found.\n");
    } else if (GetConsistencyChecking () &&
               (ReportDumpBinFile != ReportOption)) {
      OriginalAttribute = gST->ConOut->Mode->Attribute;

      Print (L"\nTable Statistics:\n");

      if (GetColourHighlighting ()) {
        PrintAttribute = (GetErrorCount () > 0) ?
                            EFI_TEXT_ATTR (
                              EFI_RED,
                              ((OriginalAttribute&(BIT4|BIT5|BIT6))>>4)
                              ) :
                            OriginalAttribute;
        gST->ConOut->SetAttribute (gST->ConOut, PrintAttribute);
      }
      Print (L"\t%d Error(s)\n", GetErrorCount ());

      if (GetColourHighlighting ()) {
        PrintAttribute = (GetWarningCount () > 0) ?
                            EFI_TEXT_ATTR (
                              EFI_RED,
                              ((OriginalAttribute&(BIT4|BIT5|BIT6))>>4)
                              ) :
                            OriginalAttribute;

        gST->ConOut->SetAttribute (gST->ConOut, PrintAttribute);
      }
      Print (L"\t%d Warning(s)\n", GetWarningCount ());

      if (GetColourHighlighting ()) {
        gST->ConOut->SetAttribute (gST->ConOut, OriginalAttribute);
      }
    }
  }
  return EFI_SUCCESS;
}

/**
  Function for 'acpiview' command.

  @param[in] ImageHandle  Handle to the Image (NULL if Internal).
  @param[in] SystemTable  Pointer to the System Table (NULL if Internal).
**/
SHELL_STATUS
EFIAPI
ShellCommandRunAcpiView (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE* SystemTable
  )
{
  EFI_STATUS         Status;
  SHELL_STATUS       ShellStatus;
  LIST_ENTRY*        Package;
  CHAR16*            ProblemParam;
  SHELL_FILE_HANDLE  TmpDumpFileHandle;
  CONST CHAR16*      MandatoryTableSpecStr;

  // Set Defaults
  mReportType = ReportAll;
  mTableCount = 0;
  mBinTableCount = 0;
  mSelectedAcpiTable = 0;
  mSelectedAcpiTableName = NULL;
  mSelectedAcpiTableFound = FALSE;
  mConsistencyCheck = TRUE;
  mMandatoryTableValidate = FALSE;
  mMandatoryTableSpec = 0;

  ShellStatus = SHELL_SUCCESS;
  Package = NULL;
  TmpDumpFileHandle = NULL;

  // Reset The error/warning counters
  ResetErrorCount ();
  ResetWarningCount ();

  Status = ShellCommandLineParse (ParamList, &Package, &ProblemParam, TRUE);
  if (EFI_ERROR (Status)) {
    if (Status == EFI_VOLUME_CORRUPTED && ProblemParam != NULL) {
      ShellPrintHiiEx (
        -1,
        -1,
        NULL,
        STRING_TOKEN (STR_GEN_PROBLEM),
        gShellAcpiViewHiiHandle,
        L"acpiview",
        ProblemParam
        );
      FreePool (ProblemParam);
    } else {
      Print (L"acpiview: Error processing input parameter(s)\n");
    }
    ShellStatus = SHELL_INVALID_PARAMETER;
  } else {
    if (ShellCommandLineGetCount (Package) > 1) {
      ShellPrintHiiEx (
        -1,
        -1,
        NULL,
        STRING_TOKEN (STR_GEN_TOO_MANY),
        gShellAcpiViewHiiHandle,
        L"acpiview"
        );
      ShellStatus = SHELL_INVALID_PARAMETER;
    } else if (ShellCommandLineGetFlag (Package, L"-?")) {
      ShellPrintHiiEx (
        -1,
        -1,
        NULL,
        STRING_TOKEN (STR_GET_HELP_ACPIVIEW),
        gShellAcpiViewHiiHandle,
        L"acpiview"
        );
    } else if (ShellCommandLineGetFlag (Package, L"-s") &&
               ShellCommandLineGetValue (Package, L"-s") == NULL) {
      ShellPrintHiiEx (
        -1,
        -1,
        NULL,
        STRING_TOKEN (STR_GEN_NO_VALUE),
        gShellAcpiViewHiiHandle,
        L"acpiview",
        L"-s"
        );
      ShellStatus = SHELL_INVALID_PARAMETER;
    } else if (ShellCommandLineGetFlag (Package, L"-r") &&
               ShellCommandLineGetValue (Package, L"-r") == NULL) {
      ShellPrintHiiEx (
        -1,
        -1,
        NULL,
        STRING_TOKEN (STR_GEN_NO_VALUE),
        gShellAcpiViewHiiHandle,
        L"acpiview",
        L"-r"
        );
      ShellStatus = SHELL_INVALID_PARAMETER;
    } else if ((ShellCommandLineGetFlag (Package, L"-s") &&
                ShellCommandLineGetFlag (Package, L"-l"))) {
      ShellPrintHiiEx (
        -1,
        -1,
        NULL,
        STRING_TOKEN (STR_GEN_TOO_MANY),
        gShellAcpiViewHiiHandle,
        L"acpiview"
        );
      ShellStatus = SHELL_INVALID_PARAMETER;
    } else if (ShellCommandLineGetFlag (Package, L"-d") &&
               !ShellCommandLineGetFlag (Package, L"-s")) {
        ShellPrintHiiEx (
          -1,
          -1,
          NULL,
          STRING_TOKEN (STR_GEN_MISSING_OPTION),
          gShellAcpiViewHiiHandle,
          L"acpiview",
          L"-s",
          L"-d"
          );
        ShellStatus = SHELL_INVALID_PARAMETER;
    } else {
      // Turn on colour highlighting if requested
      SetColourHighlighting (ShellCommandLineGetFlag (Package, L"-h"));

      // Surpress consistency checking if requested
      SetConsistencyChecking (!ShellCommandLineGetFlag (Package, L"-q"));

      // Evaluate the parameters for mandatory ACPI table presence checks
      SetMandatoryTableValidate (ShellCommandLineGetFlag (Package, L"-r"));
      MandatoryTableSpecStr = ShellCommandLineGetValue (Package, L"-r");

      if (MandatoryTableSpecStr != NULL) {
        SetMandatoryTableSpec (ShellHexStrToUintn (MandatoryTableSpecStr));
      }

      if (ShellCommandLineGetFlag (Package, L"-l")) {
        mReportType = ReportTableList;
      } else {
        mSelectedAcpiTableName = ShellCommandLineGetValue (Package, L"-s");
        if (mSelectedAcpiTableName != NULL) {
          mSelectedAcpiTable = (UINT32)ConvertStrToAcpiSignature (
                                         mSelectedAcpiTableName
                                         );
          mReportType = ReportSelected;

          if (ShellCommandLineGetFlag (Package, L"-d"))  {
            // Create a temporary file to check if the media is writable.
            CHAR16 FileNameBuffer[MAX_FILE_NAME_LEN];
            mReportType = ReportDumpBinFile;

            UnicodeSPrint (
              FileNameBuffer,
              sizeof (FileNameBuffer),
              L".\\%s%04d.tmp",
              mSelectedAcpiTableName,
              mBinTableCount
              );

            Status = ShellOpenFileByName (
                       FileNameBuffer,
                       &TmpDumpFileHandle,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                       EFI_FILE_MODE_CREATE,
                       0
                       );

            if (EFI_ERROR (Status)) {
              ShellStatus = SHELL_INVALID_PARAMETER;
              TmpDumpFileHandle = NULL;
              ShellPrintHiiEx (
                -1,
                -1,
                NULL,
                STRING_TOKEN (STR_GEN_READONLY_MEDIA),
                gShellAcpiViewHiiHandle,
                L"acpiview"
                );
              goto Done;
            }
            // Delete Temporary file.
            ShellDeleteFile (&TmpDumpFileHandle);
          } // -d
        } // -s
      }

      // Parse ACPI Table information
      Status = AcpiView (SystemTable);
      if (EFI_ERROR (Status)) {
        ShellStatus = SHELL_NOT_FOUND;
      }
    }
  }

Done:
  if (Package != NULL) {
    ShellCommandLineFreeVarList (Package);
  }
  return ShellStatus;
}
