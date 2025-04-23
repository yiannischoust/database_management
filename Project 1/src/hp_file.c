#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bf.h"
#include "hp_file.h"
#include "record.h"

enum HP_RETURN { HP_OK =0, HP_ERROR=-1 };
#define CALL_BF(call)       \
{                           \
  BF_ErrorCode code = call; \
  if (code != BF_OK) {         \
    BF_PrintError(code);    \
    return HP_ERROR;        \
  }                         \
}


int HP_CreateFile(char *fileName){
  BF_Block *block; 
  BF_Block_Init(&block); //initalize a block
  int fileds=0;


  CALL_BF(BF_CreateFile(fileName));
  CALL_BF(BF_OpenFile(fileName, &fileds)); 
  CALL_BF(BF_AllocateBlock(fileds, block)); //allocate new block at the end of the file

  HP_info *metadata = (HP_info *)BF_Block_GetData(block); // create a pointer at the start of the block to store metadata for the database

  metadata->number_of_records = 0;  
  metadata->last_block_id = 0;                               
  metadata->file_name = fileName;  
  metadata->max_records_in_a_block = BF_BLOCK_SIZE / sizeof(Record);  
  metadata->num_of_last_block_records = metadata->max_records_in_a_block;     // first block is declared filled so as to not put any data into it besides the metadata

  
  BF_Block_SetDirty(block); 
  CALL_BF(BF_UnpinBlock(block));                                  
  CALL_BF(BF_CloseFile(fileds));                                                  
  BF_Block_Destroy(&block);                     

  return  HP_OK;
}



HP_info* HP_OpenFile(char *fileName, int *file_desc){
  HP_info* metadata;  

  BF_OpenFile(fileName, file_desc);   
  BF_Block* headerBlock;                                         
  BF_Block_Init(&headerBlock);
  BF_GetBlock(*file_desc, 0, headerBlock);    

  metadata = (HP_info *)BF_Block_GetData(headerBlock);     // Pointer at metadata where the data of the block stores it 
  BF_Block_Destroy(&headerBlock);                                //Destroys the block data structure now that we are done

  return metadata;     
}


int HP_CloseFile(int file_desc,HP_info* metadata ){
  BF_Block* headerBlock;                                         
  BF_Block_Init(&headerBlock);

  BF_GetBlock(file_desc, 0, headerBlock);                         // Retrieves the header block      

  BF_Block_SetDirty(headerBlock);                                // Since the file closes we no longer will need this header block in memory
  CALL_BF(BF_UnpinBlock(headerBlock));                       
  BF_Block_Destroy(&headerBlock);                                // free memory of header block

  CALL_BF(BF_CloseFile(file_desc));      


  return HP_OK;
  
}


int HP_InsertEntry(int file_desc,HP_info* metadata, Record record){

    BF_Block *block;
    BF_Block_Init(&block);                                                          //allocate memory to store a block
    int block_number;
    if(metadata->num_of_last_block_records >= metadata->max_records_in_a_block) {   // Check if the last block is full -- it only check the last block for free space
        CALL_BF(BF_AllocateBlock(file_desc, block));                                // if so, add a new block at the end of the file
        Record* rec_entry = (Record*)BF_Block_GetData(block);                       // rec_entry Record pointer at the start of the block
        memcpy(&rec_entry[0], &record, sizeof(record));                             //insert new entry record at the start of the block
        
        CALL_BF(BF_GetBlockCounter(file_desc, &block_number));
        metadata->last_block_id = block_number - 1;                                     // update HP_info metadata about the last block id
        metadata->num_of_last_block_records = 1;                                        // number of entries at the last block set to 1
    }
    else {                                                                              //else, if the last block is not full
        CALL_BF(BF_GetBlock(file_desc, metadata->last_block_id, block));
                                                     
        Record* rec_entry = (Record*)BF_Block_GetData(block);                               // rec_entry Record pointer at the start of the block
        memcpy(&rec_entry[metadata->num_of_last_block_records], &record, sizeof(record));   // insert entry at the num_of_last_block_records position of the block
        metadata->num_of_last_block_records++;                                             // number of entries at the last block increased by 1
        
    }
    BF_GetBlockCounter(file_desc, &block_number);                   // get block_number for the function return
    BF_Block_SetDirty(block);                                       // set block as dirty, as it has been changed
    CALL_BF(BF_UnpinBlock(block));                                  // unpin block as we are done working on it
    BF_Block_Destroy(&block);                                       // free block from memory
    return block_number;
}



int HP_GetAllEntries(int file_desc,HP_info* metadata, int value){   
  
  int block_number; 
  CALL_BF(BF_GetBlockCounter(file_desc, &block_number));

  BF_Block* block;                                               
  BF_Block_Init(&block);

  for(int current_block = 1; current_block < block_number; current_block++){                          

    CALL_BF(BF_GetBlock(file_desc, current_block, block)); 
    Record* rec_entry = (Record*)BF_Block_GetData(block);              // rec_entry Record pointer at the start of the block

    for(int i = 0; i < metadata->max_records_in_a_block; i++){                        // Look through all records in a block
      if(rec_entry[i].id == value){                                       
        printRecord(rec_entry[i]);                                     // Prints out any entry that matches the id
      }
    }
    CALL_BF(BF_UnpinBlock(block));                           
  }
  BF_Block_Destroy(&block);                                      
  return block_number;       
}